// SPDX-License-Identifier: MIT
// `conversation` namespace: authoring for CommonConversation graph assets.
//
// Engine-generic. Node creation goes through FAISchemaAction_NewNode (what the graph editor's context
// menu runs), so every node gets its runtime instance and pins exactly as if it was placed by hand.
// The finished graph is compiled with FConversationCompiler::RebuildBank. Game-thread only.

#include "MonolithConversationActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "AIGraphTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ConversationDatabase.h"
#include "ConversationCompiler.h"
#include "ConversationEntryPointNode.h"
#include "ConversationGraph.h"
#include "ConversationGraphNode_EntryPoint.h"
#include "ConversationGraphNode_Task.h"
#include "ConversationTaskNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "GameplayTagContainer.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace MonolithConversationInternal
{
	constexpr int32 ErrInvalidParams = -32602;
	constexpr int32 MaxNodes = 256;
	constexpr int32 NodeSpacingX = 360;

	FMonolithActionResult BadParam(const TCHAR* Field, const FString& Detail)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), Field, *Detail), ErrInvalidParams);
	}

	/** A graph that has been loaded the way the Conversation editor loads it. */
	UConversationGraph* EnsureGraph(UConversationDatabase* Database)
	{
		if (FConversationCompiler::GetNumGraphs(Database) == 0)
		{
			FConversationCompiler::AddNewGraph(Database, TEXT("ConversationGraph"));
		}

		UConversationGraph* Graph = FConversationCompiler::GetGraphFromBank(Database, 0);
		if (Graph)
		{
			Graph->OnLoaded();
			Graph->Initialize();
			Graph->UpdateAsset(0);
		}
		return Graph;
	}

	bool SaveDatabase(UConversationDatabase* Database, FString& OutError)
	{
		UPackage* Package = Database->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.Error = GWarn; // GError (the default) is fatal on a failed save
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Database, *Filename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *Filename);
			return false;
		}
		return true;
	}

	/** Short name ("MyTask" or "UMyTask") or full path of a concrete UConversationTaskNode subclass. */
	UClass* ResolveTaskClass(const FString& Name, FString& OutError)
	{
		UClass* Class = nullptr;
		if (Name.Contains(TEXT("/")))
		{
			Class = LoadClass<UConversationTaskNode>(nullptr, *Name);
		}
		else
		{
			Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (!Class && Name.StartsWith(TEXT("U")))
			{
				Class = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!Class)
		{
			OutError = FString::Printf(TEXT("Task class '%s' was not found"), *Name);
			return nullptr;
		}
		if (!Class->IsChildOf(UConversationTaskNode::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("'%s' is not a concrete ConversationTaskNode class"), *Name);
			return nullptr;
		}
		return Class;
	}

	/** Strings, numbers and booleans, as the text ImportText expects. */
	bool JsonValueToImportText(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (Value->TryGetString(OutText))
		{
			return true;
		}

		bool bBool = false;
		if (Value->TryGetBool(bBool))
		{
			OutText = bBool ? TEXT("True") : TEXT("False");
			return true;
		}

		double Number = 0.0;
		if (Value->TryGetNumber(Number))
		{
			OutText = FString::SanitizeFloat(Number);
			return true;
		}
		return false;
	}

	/** Sets each property with ImportText, the same text format the editor's details panel uses. */
	bool ApplyProperties(UObject* Object, const TSharedPtr<FJsonObject>& Properties, FString& OutError)
	{
		if (!Properties.IsValid())
		{
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*Pair.Key));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property '%s'"), *Object->GetClass()->GetName(), *Pair.Key);
				return false;
			}

			FString Text;
			if (!Pair.Value.IsValid() || !JsonValueToImportText(Pair.Value, Text))
			{
				OutError = FString::Printf(TEXT("Property '%s' needs a string, number or boolean value"), *Pair.Key);
				return false;
			}

			Object->Modify();
			if (!Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None))
			{
				OutError = FString::Printf(TEXT("Could not import '%s' into property '%s'"), *Text, *Pair.Key);
				return false;
			}
		}
		return true;
	}

	UEdGraphPin* FindPin(const UEdGraphNode* Node, const EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Places a node through the same schema action as the editor's context menu, wired to FromPin if given. */
	UAIGraphNode* PlaceNode(UConversationGraph* Graph, const TSubclassOf<UAIGraphNode> EditorNodeClass,
		UClass* RuntimeClass, UEdGraphPin* FromPin, const FVector2f& Location)
	{
		UAIGraphNode* Template = NewObject<UAIGraphNode>(Graph, EditorNodeClass);
		Template->ClassData = FGraphNodeClassData(RuntimeClass, FString());

		FAISchemaAction_NewNode Action;
		Action.NodeTemplate = Template;
		return Cast<UAIGraphNode>(Action.PerformAction(Graph, FromPin, Location, false));
	}

	void ClearGraph(UConversationGraph* Graph)
	{
		const TArray<TObjectPtr<UEdGraphNode>> OldNodes = Graph->Nodes;
		for (UEdGraphNode* Node : OldNodes)
		{
			Graph->RemoveNode(Node);
		}
	}

	bool SetSpeakers(UConversationDatabase* Database, const TArray<TSharedPtr<FJsonValue>>& Speakers, FString& OutError)
	{
		FArrayProperty* Property = FindFProperty<FArrayProperty>(UConversationDatabase::StaticClass(), TEXT("Speakers"));
		if (!Property)
		{
			OutError = TEXT("ConversationDatabase::Speakers was not found");
			return false;
		}

		FScriptArrayHelper Array(Property, Property->ContainerPtrToValuePtr<void>(Database));
		Database->Modify();
		Array.EmptyValues();

		for (const TSharedPtr<FJsonValue>& Value : Speakers)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString TagName;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("tag"), TagName))
			{
				OutError = TEXT("Each speaker must be an object with a 'tag' string");
				return false;
			}

			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), false);
			if (!Tag.IsValid())
			{
				OutError = FString::Printf(TEXT("Speaker tag '%s' is not a registered gameplay tag"), *TagName);
				return false;
			}

			const int32 Index = Array.AddValue();
			auto* Speaker = reinterpret_cast<FCommonDialogueBankParticipant*>(Array.GetRawPtr(Index));
			Speaker->ParticipantName = Tag;

			FString Name;
			if ((*Object)->TryGetStringField(TEXT("name"), Name))
			{
				Speaker->FallbackName = FText::FromString(Name);
			}
		}
		return true;
	}

	/** Compiled-data counters, read by reflection because the database keeps them private. */
	int32 CountReflected(UConversationDatabase* Database, const TCHAR* PropertyName)
	{
		FProperty* Property = Database->GetClass()->FindPropertyByName(FName(PropertyName));
		if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			return FScriptMapHelper(Map, Map->ContainerPtrToValuePtr<void>(Database)).Num();
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			return FScriptArrayHelper(Array, Array->ContainerPtrToValuePtr<void>(Database)).Num();
		}
		return INDEX_NONE;
	}

	TSharedPtr<FJsonObject> MakeCompiledJson(UConversationDatabase* Database)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("reachable_node_count"), CountReflected(Database, TEXT("ReachableNodeMap")));
		Json->SetNumberField(TEXT("entry_tag_count"), CountReflected(Database, TEXT("EntryTags")));
		return Json;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithConversationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("conversation"), TEXT("create_conversation_database"),
		TEXT("Create an empty ConversationDatabase asset with its source graph initialised, so it opens in the Conversation editor. Fails if the asset exists. Returns {asset_path, saved}."),
		FMonolithActionHandler::CreateStatic(&FMonolithConversationActions::CreateConversationDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path of the new asset, e.g. /Game/Conversations/CD_Intro"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the new asset to disk."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("conversation"), TEXT("build_from_spec"),
		TEXT("Replace a ConversationDatabase's graph with a linear chain: Entry Point -> Task -> Task ..., set its speakers, compile it and optionally save. Existing nodes in the graph are removed. The entry tag and speaker tags must be registered gameplay tags. nodes[] are {class, properties?}: class is a concrete ConversationTaskNode subclass (short name or path); properties maps property names to ImportText-format strings (numbers and booleans are accepted). Returns {asset_path, node_count, nodes:[{index, class, node_guid}], compiled:{reachable_node_count, entry_tag_count}, saved}."),
		FMonolithActionHandler::CreateStatic(&FMonolithConversationActions::BuildFromSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path of an existing ConversationDatabase"))
			.Required(TEXT("entry_tag"), TEXT("string"), TEXT("Gameplay tag of the Entry Point, e.g. Conversation.Entry.Intro"))
			.Required(TEXT("nodes"), TEXT("array"), TEXT("Task chain in order: [{class, properties?}] (1-256 nodes)"))
			.Optional(TEXT("speakers"), TEXT("array"), TEXT("Replaces the speaker list: [{tag, name?}]. Omit to keep the current speakers."))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the asset after compiling."), TEXT("true"))
			.Build());
}

// ============================================================
//  create_conversation_database
// ============================================================

FMonolithActionResult FMonolithConversationActions::CreateConversationDatabase(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithConversationInternal;

	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return BadParam(TEXT("asset_path"), TEXT("required string"));
	}

	const FString PathError = MonolithCore::ValidatePackagePath(AssetPath);
	if (!PathError.IsEmpty())
	{
		return BadParam(TEXT("asset_path"), PathError);
	}
	if (FMonolithAssetUtils::AssetExists(AssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' already exists"), *AssetPath), ErrInvalidParams);
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	UPackage* Package = CreatePackage(*AssetPath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not create package '%s'"), *AssetPath));
	}

	auto* Database = NewObject<UConversationDatabase>(Package, *FPackageName::GetShortName(AssetPath),
		RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Database);

	if (!EnsureGraph(Database))
	{
		return FMonolithActionResult::Error(TEXT("Could not create the source graph"));
	}
	Database->MarkPackageDirty();

	FString SaveError;
	if (bSave && !SaveDatabase(Database, SaveError))
	{
		return FMonolithActionResult::Error(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("saved"), bSave);
	return FMonolithActionResult::Success(Result);
}

// ============================================================
//  build_from_spec
// ============================================================

namespace MonolithConversationInternal
{
	struct FTaskSpec
	{
		UClass* Class = nullptr;
		TSharedPtr<FJsonObject> Properties;
	};

	/** Validates every node spec up front, so a bad spec never leaves a half-built graph behind. */
	bool ParseTaskSpecs(const TArray<TSharedPtr<FJsonValue>>& Nodes, TArray<FTaskSpec>& OutSpecs, FString& OutError)
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString ClassName;
			if (!Nodes[Index].IsValid() || !Nodes[Index]->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("class"), ClassName))
			{
				OutError = FString::Printf(TEXT("nodes[%d] must be an object with a 'class' string"), Index);
				return false;
			}

			FTaskSpec& Spec = OutSpecs.AddDefaulted_GetRef();
			FString ClassError;
			Spec.Class = ResolveTaskClass(ClassName, ClassError);
			if (!Spec.Class)
			{
				OutError = FString::Printf(TEXT("nodes[%d]: %s"), Index, *ClassError);
				return false;
			}

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if ((*Object)->TryGetObjectField(TEXT("properties"), Properties))
			{
				Spec.Properties = *Properties;
			}
		}
		return true;
	}
}

FMonolithActionResult FMonolithConversationActions::BuildFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithConversationInternal;

	FString AssetPath;
	FString EntryTagName;
	const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return BadParam(TEXT("asset_path"), TEXT("required string"));
	}
	if (!Params->TryGetStringField(TEXT("entry_tag"), EntryTagName))
	{
		return BadParam(TEXT("entry_tag"), TEXT("required string"));
	}
	if (!Params->TryGetArrayField(TEXT("nodes"), NodeValues) || NodeValues->IsEmpty() || NodeValues->Num() > MaxNodes)
	{
		return BadParam(TEXT("nodes"), FString::Printf(TEXT("required array of 1-%d nodes"), MaxNodes));
	}

	const FGameplayTag EntryTag = FGameplayTag::RequestGameplayTag(FName(*EntryTagName), false);
	if (!EntryTag.IsValid())
	{
		return BadParam(TEXT("entry_tag"), FString::Printf(TEXT("'%s' is not a registered gameplay tag"), *EntryTagName));
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);

	auto* Database = FMonolithAssetUtils::LoadAssetByPath<UConversationDatabase>(AssetPath);
	if (!Database)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("'%s' is not a ConversationDatabase"), *AssetPath), ErrInvalidParams);
	}

	TArray<FTaskSpec> Specs;
	FString Error;
	if (!ParseTaskSpecs(*NodeValues, Specs, Error))
	{
		return BadParam(TEXT("nodes"), Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* SpeakerValues = nullptr;
	if (Params->TryGetArrayField(TEXT("speakers"), SpeakerValues) && !SetSpeakers(Database, *SpeakerValues, Error))
	{
		return BadParam(TEXT("speakers"), Error);
	}

	UConversationGraph* Graph = EnsureGraph(Database);
	if (!Graph)
	{
		return FMonolithActionResult::Error(TEXT("Could not load the source graph"));
	}
	ClearGraph(Graph);

	// Entry Point
	UAIGraphNode* EntryNode = PlaceNode(Graph, UConversationGraphNode_EntryPoint::StaticClass(),
		UConversationEntryPointNode::StaticClass(), nullptr, FVector2f(0.f, 0.f));
	auto* EntryInstance = EntryNode ? Cast<UConversationEntryPointNode>(EntryNode->NodeInstance) : nullptr;
	if (!EntryInstance)
	{
		return FMonolithActionResult::Error(TEXT("Could not create the Entry Point node"));
	}
	EntryInstance->Modify();
	EntryInstance->EntryTag = EntryTag;

	// Tasks, each wired to the previous node's output
	TArray<TSharedPtr<FJsonValue>> NodeResults;
	UEdGraphNode* Previous = EntryNode;
	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		UEdGraphPin* FromPin = FindPin(Previous, EGPD_Output);
		UAIGraphNode* Node = FromPin ? PlaceNode(Graph, UConversationGraphNode_Task::StaticClass(), Specs[Index].Class,
			FromPin, FVector2f(static_cast<float>((Index + 1) * NodeSpacingX), 0.f)) : nullptr;
		if (!Node || !Node->NodeInstance || FromPin->LinkedTo.IsEmpty())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Could not create and connect nodes[%d]"), Index));
		}
		if (!ApplyProperties(Node->NodeInstance, Specs[Index].Properties, Error))
		{
			return BadParam(TEXT("nodes"), FString::Printf(TEXT("nodes[%d]: %s"), Index, *Error));
		}

		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetNumberField(TEXT("index"), Index);
		NodeJson->SetStringField(TEXT("class"), Specs[Index].Class->GetName());
		NodeJson->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeResults.Add(MakeShared<FJsonValueObject>(NodeJson));
		Previous = Node;
	}

	Graph->UpdateAsset(0);
	FConversationCompiler::RebuildBank(Database);
	Database->MarkPackageDirty();

	if (bSave && !SaveDatabase(Database, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetNumberField(TEXT("node_count"), Specs.Num());
	Result->SetArrayField(TEXT("nodes"), NodeResults);
	Result->SetObjectField(TEXT("compiled"), MakeCompiledJson(Database));
	Result->SetBoolField(TEXT("saved"), bSave);
	return FMonolithActionResult::Success(Result);
}
