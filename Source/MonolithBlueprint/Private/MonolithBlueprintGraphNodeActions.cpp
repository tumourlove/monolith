#include "MonolithBlueprintGraphNodeActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "EdGraph/EdGraphNode.h"
#include "ScopedTransaction.h"

// ---------------------------------------------------------------------------
// Shared: resolve a UEdGraph sub-object node by its full path.
// Accepts the ExportPath form emitted by project.export_asset_text, e.g.
//   /Game/Foo/CO_David.CO_David:CustomizableObjectGraph_0.CustomizableObjectNodeSkeletalMesh_1
// FSoftObjectPath::TryLoad resolves the ':sub.sub' suffix once the outer asset
// is (or gets) loaded.
// ---------------------------------------------------------------------------
namespace MonolithGraphNodeInternal
{
	static UObject* ResolveNodeObject(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		// TryLoad handles both top-level and sub-object paths; falls back to a
		// find for already-resident objects.
		UObject* Obj = FSoftObjectPath(ObjectPath).TryLoad();
		if (!Obj)
		{
			Obj = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
		}
		return Obj;
	}

	static FProperty* FindProp(UClass* Cls, const FString& PropertyName)
	{
		FProperty* Prop = Cls->FindPropertyByName(FName(*PropertyName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(Cls); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}
		return Prop;
	}
}

void FMonolithBlueprintGraphNodeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_graph_node_property"),
		TEXT("Set a UPROPERTY on a single node INSIDE an asset's graph (addressed by its full "
			 "sub-object path) via reflection, then call UEdGraphNode::ReconstructNode() so the "
			 "node rebuilds its pins/sections. Engine-generic: works for any UEdGraphNode in any "
			 "graph (Mutable CustomizableObject, AnimGraph, Material, Niagara, Blueprint). Primary "
			 "use: populate a Mutable CustomizableObjectNodeSkeletalMesh whose concrete class is in "
			 "a /Private/ header and can't be typed directly — e.g. object_path "
			 "'/Game/.../CO_David.CO_David:CustomizableObjectGraph_0.CustomizableObjectNodeSkeletalMesh_1', "
			 "property_name 'SkeletalMesh', value '/Game/.../SKM_David_BodyMesh.SKM_David_BodyMesh'. "
			 "Get node names/paths from project.export_asset_text or get_graph_node_properties."),
		FMonolithActionHandler::CreateStatic(&FMonolithBlueprintGraphNodeActions::HandleSetGraphNodeProperty),
		FParamSchemaBuilder()
			.Required(TEXT("object_path"), TEXT("string"), TEXT("Full sub-object path of the graph node, e.g. '/Game/Foo/CO.CO:CustomizableObjectGraph_0.NodeSkeletalMesh_1'."))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("UPROPERTY name to set (case-insensitive fallback), e.g. 'SkeletalMesh'."))
			.Required(TEXT("value"), TEXT("any"), TEXT("New value — an asset path for object/soft-object props, a scalar/enum name, or ImportText/JSON for structs & collections."))
			.Optional(TEXT("reconstruct"), TEXT("boolean"), TEXT("Call UEdGraphNode::ReconstructNode() after the write to rebuild pins/sections. Default true — required for mesh/section changes to take effect."), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the owning package to disk after the edit (UPackage::SavePackage). Default true. False = MarkPackageDirty only."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_graph_node_properties"),
		TEXT("Read the editable UPROPERTYs of a single graph node addressed by its full sub-object "
			 "path (companion to set_graph_node_property). Returns the node class plus each property's "
			 "name, type and current exported value. Use to discover the exact property name (e.g. "
			 "'SkeletalMesh') and verify a write."),
		FMonolithActionHandler::CreateStatic(&FMonolithBlueprintGraphNodeActions::HandleGetGraphNodeProperties),
		FParamSchemaBuilder()
			.Required(TEXT("object_path"), TEXT("string"), TEXT("Full sub-object path of the graph node."))
			.Build());
}

FMonolithActionResult FMonolithBlueprintGraphNodeActions::HandleSetGraphNodeProperty(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("set_graph_node_property requires params"));
	}

	const FString ObjectPath = Params->GetStringField(TEXT("object_path"));
	if (ObjectPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: object_path"));
	}
	const FString PropertyName = Params->GetStringField(TEXT("property_name"));
	if (PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: property_name"));
	}
	if (!Params->HasField(TEXT("value")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: value"));
	}
	const TSharedPtr<FJsonValue> JsonVal = Params->TryGetField(TEXT("value"));
	if (!JsonVal.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("'value' field missing or null"));
	}

	bool bReconstruct = true;
	Params->TryGetBoolField(TEXT("reconstruct"), bReconstruct);
	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	UObject* Node = MonolithGraphNodeInternal::ResolveNodeObject(ObjectPath);
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Graph node not found at path: %s (ensure the owning asset exists and the sub-object path is exact)"), *ObjectPath));
	}
	UClass* NodeClass = Node->GetClass();

	FProperty* Prop = MonolithGraphNodeInternal::FindProp(NodeClass, PropertyName);
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s"), *PropertyName, *NodeClass->GetName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Node, PPF_None);

	// --- Edit cradle: mirror the Details-panel write path so PostEditChange fires
	//     the node's own handler (which, for mesh nodes, remaps pins/sections). ---
	Node->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintGraphNodeActions", "SetGraphNodeProperty", "Monolith Set Graph Node Property"));
	Node->Modify();

	FEditPropertyChain PropertyChain;
	PropertyChain.AddHead(Prop);
	PropertyChain.SetActivePropertyNode(Prop);
	Node->PreEditChange(PropertyChain);

	// --- Set the value (JSON-aware for structs/arrays/maps; ImportText for scalars,
	//     enums, and object/soft-object asset paths) ---
	if (JsonVal->Type == EJson::Object || JsonVal->Type == EJson::Array)
	{
		if (!FJsonObjectConverter::JsonValueToUProperty(JsonVal, Prop, ValuePtr, 0, 0))
		{
			FString JsonStr;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonStr);
			FJsonSerializer::Serialize(JsonVal.ToSharedRef(), TEXT("value"), Writer);
			Writer->Close();
			if (JsonStr.Len() > 500) { JsonStr = JsonStr.Left(500) + TEXT("..."); }
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' from JSON — FJsonObjectConverter rejected the format. Preview: %s"),
				*PropertyName, *JsonStr));
		}
	}
	else
	{
		FString ValStr;
		if (JsonVal->Type == EJson::Number)
		{
			ValStr = FString::SanitizeFloat(JsonVal->AsNumber());
		}
		else if (JsonVal->Type == EJson::Boolean)
		{
			ValStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
		}
		else
		{
			ValStr = JsonVal->AsString();
		}

		const TCHAR* ImportResult = Prop->ImportText_Direct(*ValStr, ValuePtr, Node, PPF_None);
		if (!ImportResult)
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' to value '%s' — ImportText rejected the format. "
					 "For object/soft-object props pass a full asset path (e.g. '/Game/.../SKM_X.SKM_X'); "
					 "for structs use ImportText syntax; for enums use the value name."),
				*PropertyName, *ValStr));
		}
	}

	// --- Fire the change so the node reacts (mesh nodes reconstruct sections here) ---
	FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(PropertyChain, ChangeEvent);
	Node->PostEditChangeChainProperty(ChainEvent);

	// --- Reconstruct pins. This is the capability raw property-setting (e.g. Python)
	//     lacks: without it, a newly-assigned mesh leaves the node's pins/sections
	//     stale and the graph fails to compile. ReconstructNode() is virtual on the
	//     public UEdGraphNode base, so the concrete (private) node override runs. ---
	bool bReconstructed = false;
	if (bReconstruct)
	{
		if (UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Node))
		{
			GraphNode->ReconstructNode();
			bReconstructed = true;
		}
	}

	FString NewValue;
	Prop->ExportText_Direct(NewValue, ValuePtr, ValuePtr, Node, PPF_None);

	Node->MarkPackageDirty();

	// --- Optional save to disk ---
	bool bSaved = false;
	FString SaveError;
	if (bSave)
	{
		UPackage* Package = Node->GetPackage();
		if (Package)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			bSaved = UPackage::SavePackage(Package, nullptr, *PackageFilename, SaveArgs);
			if (!bSaved)
			{
				SaveError = FString::Printf(TEXT("SavePackage failed for %s"), *PackageFilename);
			}
		}
		else
		{
			SaveError = TEXT("node has no package to save");
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("object_path"), ObjectPath);
	Root->SetStringField(TEXT("node_class"), NodeClass->GetName());
	Root->SetStringField(TEXT("property_name"), Prop->GetName());
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), NewValue);
	Root->SetBoolField(TEXT("reconstructed"), bReconstructed);
	Root->SetBoolField(TEXT("saved"), bSaved);
	if (!SaveError.IsEmpty())
	{
		Root->SetStringField(TEXT("save_error"), SaveError);
	}
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithBlueprintGraphNodeActions::HandleGetGraphNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("get_graph_node_properties requires params"));
	}
	const FString ObjectPath = Params->GetStringField(TEXT("object_path"));
	if (ObjectPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: object_path"));
	}

	UObject* Node = MonolithGraphNodeInternal::ResolveNodeObject(ObjectPath);
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph node not found at path: %s"), *ObjectPath));
	}
	UClass* NodeClass = Node->GetClass();

	TArray<TSharedPtr<FJsonValue>> PropsArray;
	for (TFieldIterator<FProperty> It(NodeClass); It; ++It)
	{
		FProperty* Prop = *It;
		// Surface only user-editable properties to keep the payload focused.
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node);
		FString Exported;
		Prop->ExportText_Direct(Exported, ValuePtr, ValuePtr, Node, PPF_None);

		TSharedPtr<FJsonObject> PInfo = MakeShared<FJsonObject>();
		PInfo->SetStringField(TEXT("name"), Prop->GetName());
		PInfo->SetStringField(TEXT("type"), Prop->GetCPPType());
		PInfo->SetStringField(TEXT("value"), Exported);
		PropsArray.Add(MakeShared<FJsonValueObject>(PInfo));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("object_path"), ObjectPath);
	Root->SetStringField(TEXT("node_class"), NodeClass->GetName());
	Root->SetArrayField(TEXT("properties"), PropsArray);
	Root->SetNumberField(TEXT("property_count"), PropsArray.Num());
	return FMonolithActionResult::Success(Root);
}
