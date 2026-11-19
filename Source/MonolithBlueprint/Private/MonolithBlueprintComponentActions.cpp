#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintComponentResolver.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkinnedAsset.h"
#include "Components/SkinnedMeshComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MonolithBlueprintCompileActions.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UObjectHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithBlueprintComponentActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_component"),
		TEXT("Add a new component to a Blueprint's construction script. Returns variable_name, class, and parent."),
		FMonolithActionHandler::CreateStatic(&HandleAddComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("Component class name (e.g. 'StaticMeshComponent')"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Variable name for the new component"))
			.Optional(TEXT("parent"), TEXT("string"), TEXT("Parent component variable name (attach as child)"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_component"),
		TEXT("Remove a component from a Blueprint's construction script. Optionally promotes children to the removed node's parent."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to remove"))
			.Optional(TEXT("promote_children"), TEXT("boolean"), TEXT("Reparent children to removed node's parent (default: true)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_component"),
		TEXT("Rename a component variable in a Blueprint."),
		FMonolithActionHandler::CreateStatic(&HandleRenameComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Current component variable name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New variable name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reparent_component"),
		TEXT("Change the parent of a component in a Blueprint. Pass empty string for new_parent to make it a root component."),
		FMonolithActionHandler::CreateStatic(&HandleReparentComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to reparent"))
			.Required(TEXT("new_parent"), TEXT("string"), TEXT("New parent component variable name, or empty string to attach to root"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on new parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_component_property"),
		TEXT("Set a property on a component template in a Blueprint via text import. Writes to Blueprint-added (SCS) "
			 "components, inherited native components, and components inherited from a parent Blueprint — the last "
			 "get an Inheritable Component Handler override on THIS Blueprint so the parent is not modified. "
			 "Returns 'source' (scs | cdo_native | ich_override) and 'persisted', which is false when the value "
			 "equalled the inherited default and the override record was pruned on compile."),
		FMonolithActionHandler::CreateStatic(&HandleSetComponentProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name or alias (Mesh, StaticMesh, CharacterMovement, Capsule, Root)"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name on the component"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as text (same format as copy/paste in Details panel)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_component"),
		TEXT("Duplicate a component in a Blueprint. The copy is attached to the same parent as the original."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Variable name for the duplicated component"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("swap_component_class"),
		TEXT("Change the class of a Blueprint-added (SCS) component in place. Keeps the variable name, variable GUID, "
			 "child nodes, attach socket and graph references; copies every property the designer changed on the old "
			 "template (values that differ from the old class defaults) onto a new template of new_class, so unchanged "
			 "properties pick up new_class's own defaults. Inheritable Component Handler overrides in child Blueprints "
			 "(loaded or not) are rebuilt for the new class with their per-child values carried over. Compiles this "
			 "Blueprint and every migrated child. Scene <-> non-scene swaps and the DefaultSceneRoot node are rejected. "
			 "Placed level instances lose per-instance component values on reinstancing: pass "
			 "preserve_level_instances=true, or use dump_instance_component_values / restore_instance_component_values."),
		FMonolithActionHandler::CreateStatic(&HandleSwapComponentClass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path that declares the component (not a child Blueprint)"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("SCS component variable name"))
			.Required(TEXT("new_class"), TEXT("string"), TEXT("New component class: native name ('SkeletalMeshComponent'), script path ('/Script/Engine.SkeletalMeshComponent'), or Blueprint component asset path"))
			.Optional(TEXT("allow_unrelated"), TEXT("boolean"), TEXT("Allow a new_class that is not a subclass of the current class (default false). Only properties present on both classes are copied, and graph calls into the old class may stop compiling"))
			.Optional(TEXT("include_child_blueprints"), TEXT("boolean"), TEXT("Migrate child Blueprint overrides of this component (default true)"))
			.Optional(TEXT("preserve_level_instances"), TEXT("boolean"), TEXT("Snapshot and re-apply per-instance values of this component on actors placed in the open editor level (default false; dirties the level)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("dump_instance_component_values"),
		TEXT("Dump the per-instance component property overrides of an actor placed in the open editor level: every editable "
			 "property whose value differs from the component's archetype (its Blueprint template or native default), as "
			 "export text. Transient, deprecated and non-editable properties are ignored; editable properties that cannot "
			 "round-trip (instanced subobjects, fixed-size arrays, EditConst, DisableEditOnInstance) are listed under 'skipped'. "
			 "Pass the returned 'values' object unchanged to restore_instance_component_values."),
		FMonolithActionHandler::CreateStatic(&HandleDumpInstanceComponentValues),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or full object path in the editor world"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Only dump this component (object name, case-insensitive)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("restore_instance_component_values"),
		TEXT("Re-apply per-instance component property values to an actor placed in the open editor level, typically the "
			 "'values' object produced by dump_instance_component_values before a swap_component_class. Each value is "
			 "imported as text and fires the property-change notification. Properties missing on the current class, "
			 "transient or not editable on an instance are skipped and reported."),
		FMonolithActionHandler::CreateStatic(&HandleRestoreInstanceComponentValues),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label, object name, or full object path in the editor world"))
			.Required(TEXT("values"), TEXT("object"), TEXT("{ \"<component name>\": { \"<property>\": \"<export text>\" } } - the 'values' field of dump_instance_component_values"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Only restore this component from 'values'"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Find an SCS_Node by variable name. Searches all nodes including root. */
static USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FName& VarName)
{
	if (!SCS) return nullptr;

	// FindSCSNode searches all nodes (AllNodes)
	return SCS->FindSCSNode(VarName);
}

/** Find a component property by exact name, then case-insensitively. */
static FProperty* MonolithFindComponentProperty(UClass* ComponentClass, const FString& PropName)
{
	if (!ComponentClass) return nullptr;

	if (FProperty* Exact = ComponentClass->FindPropertyByName(FName(*PropName)))
	{
		return Exact;
	}
	for (TFieldIterator<FProperty> It(ComponentClass); It; ++It)
	{
		if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

/** Find the parent SCS_Node of a given node, or nullptr if it is a root node. */
static USCS_Node* FindParentNode(USimpleConstructionScript* SCS, USCS_Node* ChildNode)
{
	if (!SCS || !ChildNode) return nullptr;

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (!Node) continue;
		if (Node->GetChildNodes().Contains(ChildNode))
		{
			return Node;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// add_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleAddComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript (is it an Actor Blueprint?)"));
	}

	FString ClassName = Params->GetStringField(TEXT("component_class"));
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("component_class is required"));
	}

	// Try to resolve the class — accept bare name or U-prefixed name
	UClass* CompClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!CompClass)
	{
		CompClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!CompClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component class not found: %s"), *ClassName));
	}
	if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not a UActorComponent"), *ClassName));
	}

	// Determine node name — use provided name or derive from class
	FString Name = Params->GetStringField(TEXT("component_name"));
	if (Name.IsEmpty())
	{
		// Strip 'U' prefix and 'Component' suffix for a clean default name
		Name = ClassName;
		if (Name.StartsWith(TEXT("U")))
		{
			Name = Name.Mid(1);
		}
	}

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(CompClass, FName(*Name));
	if (!NewNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create SCS node"));
	}

	// Attach socket if specified (must be set before adding to hierarchy)
	FString AttachSocket = Params->GetStringField(TEXT("attach_socket"));
	if (!AttachSocket.IsEmpty())
	{
		NewNode->AttachToName = FName(*AttachSocket);
	}

	// Attach to parent component or add as root
	FString ParentName = Params->GetStringField(TEXT("parent"));
	FString ActualParentName;

	if (!ParentName.IsEmpty())
	{
		USCS_Node* ParentNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*ParentName));
		if (!ParentNode)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent component not found: %s"), *ParentName));
		}
		ParentNode->AddChildNode(NewNode);
		ActualParentName = ParentName;
	}
	else
	{
		BP->SimpleConstructionScript->AddNode(NewNode);
		ActualParentName = TEXT("");
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Root->SetStringField(TEXT("class"), CompClass->GetName());
	Root->SetStringField(TEXT("parent"), ActualParentName);
	if (!AttachSocket.IsEmpty())
	{
		Root->SetStringField(TEXT("attach_socket"), AttachSocket);
	}
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName = Params->GetStringField(TEXT("component_name"));
	if (CompName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("component_name is required"));
	}

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Determine promote_children (default true)
	bool bPromoteChildren = true;
	{
		bool bVal = true;
		if (Params->TryGetBoolField(TEXT("promote_children"), bVal))
		{
			bPromoteChildren = bVal;
		}
	}

	FString RemovedName = Node->GetVariableName().ToString();
	FString RemovedClass = Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown");
	int32 ChildCount = Node->GetChildNodes().Num();

	if (bPromoteChildren)
	{
		BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
	}
	else
	{
		BP->SimpleConstructionScript->RemoveNode(Node);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed"), RemovedName);
	Root->SetStringField(TEXT("class"), RemovedClass);
	Root->SetNumberField(TEXT("children_promoted"), bPromoteChildren ? ChildCount : 0);
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// rename_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleRenameComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName = Params->GetStringField(TEXT("component_name"));
	FString NewName  = Params->GetStringField(TEXT("new_name"));

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));
	if (NewName.IsEmpty())  return FMonolithActionResult::Error(TEXT("new_name is required"));
	if (FName(*NewName).IsNone()) return FMonolithActionResult::Error(TEXT("'None' is a reserved FName and cannot be used as a component name"));

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Verify name is not already taken by another SCS node
	if (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewName)))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A component named '%s' already exists"), *NewName));
	}

	// Check against BP member variables
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == FName(*NewName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot rename component to '%s': a variable with that name already exists"), *NewName));
		}
	}

	// Check against function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == FName(*NewName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot rename component to '%s': a function with that name already exists"), *NewName));
		}
	}

	FBlueprintEditorUtils::RenameComponentMemberVariable(BP, Node, FName(*NewName));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), CompName);
	Root->SetStringField(TEXT("new_name"), Node->GetVariableName().ToString());
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// reparent_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleReparentComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName   = Params->GetStringField(TEXT("component_name"));
	FString NewParent  = Params->GetStringField(TEXT("new_parent"));
	FString AttachSocket = Params->GetStringField(TEXT("attach_socket"));

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Validate new parent if specified — must not be the node itself or a descendant
	USCS_Node* NewParentNode = nullptr;
	if (!NewParent.IsEmpty())
	{
		NewParentNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewParent));
		if (!NewParentNode)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("New parent component not found: %s"), *NewParent));
		}
		if (NewParentNode == Node)
		{
			return FMonolithActionResult::Error(TEXT("Cannot reparent a component to itself"));
		}
		// Check that new parent is not a child of the node we're moving (would create cycle)
		TArray<USCS_Node*> AllDescendants;
		TArray<USCS_Node*> Stack = Node->GetChildNodes();
		while (Stack.Num() > 0)
		{
			USCS_Node* Current = Stack.Pop(EAllowShrinking::No);
			if (!Current) continue;
			AllDescendants.Add(Current);
			Stack.Append(Current->GetChildNodes());
		}
		if (AllDescendants.Contains(NewParentNode))
		{
			return FMonolithActionResult::Error(TEXT("Cannot reparent component to one of its own descendants"));
		}
	}

	// Determine current parent so we know how to detach
	USCS_Node* OldParent = FindParentNode(BP->SimpleConstructionScript, Node);

	// Update attach socket
	if (!AttachSocket.IsEmpty())
	{
		Node->AttachToName = FName(*AttachSocket);
	}
	else
	{
		Node->AttachToName = NAME_None;
	}

	// Detach from current location, then reattach at new location
	if (NewParentNode)
	{
		if (OldParent)
		{
			// Detach from old parent, then attach under new parent
			OldParent->RemoveChildNode(Node);
			NewParentNode->AddChildNode(Node);
		}
		else
		{
			// Was a root node — remove from root list, then attach under new parent
			BP->SimpleConstructionScript->RemoveNode(Node);
			NewParentNode->AddChildNode(Node);
		}
	}
	else
	{
		// Target is root
		if (OldParent)
		{
			// Detach from old parent, promote to root
			OldParent->RemoveChildNode(Node);
			BP->SimpleConstructionScript->AddNode(Node);
		}
		// If already root, nothing to do
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("new_parent"), NewParent.IsEmpty() ? TEXT("(root)") : NewParent);
	if (!AttachSocket.IsEmpty())
	{
		Root->SetStringField(TEXT("attach_socket"), AttachSocket);
	}
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_component_property
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString CompName  = Params->GetStringField(TEXT("component_name"));
	FString PropName  = Params->GetStringField(TEXT("property_name"));
	FString Value     = Params->GetStringField(TEXT("value"));

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));
	if (PropName.IsEmpty()) return FMonolithActionResult::Error(TEXT("property_name is required"));

	// One resolver for the whole module — see MonolithBlueprintComponentResolver.h. Write intent
	// (bCreateIchOverride=true): a component inherited from a parent Blueprint gets THIS
	// Blueprint's own Inheritable Component Handler override so the write lands on the child
	// rather than mutating the parent's template.
	const MonolithBlueprintComponentResolver::FResult Resolved =
		MonolithBlueprintComponentResolver::Resolve(
			BP, CompName, UActorComponent::StaticClass(), /*bCreateIchOverride=*/true);

	if (!Resolved.IsValid())
	{
		return FMonolithActionResult::Error(Resolved.Error.IsEmpty()
			? FString::Printf(TEXT("Component not found: %s"), *CompName)
			: Resolved.Error);
	}

	UActorComponent* Template = Resolved.Template;

	FProperty* Prop = MonolithFindComponentProperty(Template->GetClass(), PropName);
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s"), *PropName, *Template->GetClass()->GetName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);

	// Read old value for reporting
	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Template, PPF_None);

	// Record the change for undo + ensure the CDO subobject is serialized on save.
	Template->Modify();

	// For FObjectProperty, resolve the path → load the object → assign the pointer
	// directly. ImportText is fragile for TObjectPtrs on a CDO subobject — it may
	// silently no-op if the value string isn't in the exact canonical form the
	// parser expects. Loading the object ourselves bypasses that.
	FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
	if (ObjProp)
	{
		// Accept both "/Game/.../Asset.Asset" and "Class'/Game/.../Asset.Asset'" forms.
		FString Path = Value;
		Path.TrimStartAndEndInline();
		int32 QuoteStart = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), QuoteStart))
		{
			int32 QuoteEnd = INDEX_NONE;
			if (Path.FindLastChar(TEXT('\''), QuoteEnd) && QuoteEnd > QuoteStart)
			{
				Path = Path.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			}
		}
		const bool bIsNone = Path.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Path.IsEmpty();
		UObject* NewObject = nullptr;
		if (!bIsNone)
		{
			// Load without class constraint — ObjProp->PropertyClass may be an
			// abstract base (e.g. USkinnedAsset) and asset loader can't pick a
			// concrete subclass from that. Validate compatibility after loading.
			NewObject = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), Path);
			if (!NewObject)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to load object '%s' for property '%s' (expected %s or subclass)"),
					*Path, *PropName, *ObjProp->PropertyClass->GetName()));
			}
			if (!NewObject->IsA(ObjProp->PropertyClass))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Object '%s' is a %s, not compatible with property '%s' (expects %s)"),
					*Path, *NewObject->GetClass()->GetName(), *PropName, *ObjProp->PropertyClass->GetName()));
			}
		}

		// Route the write through the canonical setter when one exists. The
		// engine contract: a UPROPERTY with `Setter=` metadata or a deprecated/
		// aliased pair (USkinnedMeshComponent::SkinnedAsset ↔ SkeletalMesh,
		// AnimClass, MeshDeformer, etc.) MUST go through the setter so all
		// aliases are written and any side effects (render-state refresh,
		// PostLoad alias re-sync) fire correctly. Writing the field directly
		// produces a value that survives serialization but is then clobbered
		// by PostLoad's alias re-sync — see SkinnedMeshComponent.cpp:660-663.
		bool bWroteValue = false;

		// 1) Special-case: USkinnedMeshComponent::SkinnedAsset is private with
		//    no Setter meta of its own; it's the new field that PostLoad copies
		//    from the deprecated SkeletalMesh. Route through SetSkinnedAssetAndUpdate
		//    which writes both aliases atomically.
		if (USkinnedMeshComponent* SMC = Cast<USkinnedMeshComponent>(Template))
		{
			if (PropName.Equals(TEXT("SkinnedAsset"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("SkeletalMeshAsset"), ESearchCase::IgnoreCase))
			{
				if (USkinnedAsset* NewAsset = Cast<USkinnedAsset>(NewObject); NewAsset || bIsNone)
				{
					SMC->SetSkinnedAssetAndUpdate(NewAsset);
					bWroteValue = true;
				}
			}
		}

		// 2) General: detect UPROPERTY `Setter=` metadata and call the named
		//    setter via reflection. This honours the engine contract for any
		//    deprecated/aliased UPROPERTY pair. Setter signature is always
		//    `void SetX(T NewValue)` so a single-pointer params struct works.
		if (!bWroteValue)
		{
			const FString SetterName = ObjProp->GetMetaData(TEXT("Setter"));
			if (!SetterName.IsEmpty())
			{
				if (UFunction* SetterFunc = Template->GetClass()->FindFunctionByName(*SetterName))
				{
					struct FObjectSetterParams { UObject* InValue; } SetterParams;
					SetterParams.InValue = NewObject;
					Template->ProcessEvent(SetterFunc, &SetterParams);
					bWroteValue = true;
				}
			}
		}

		// 3) Fallback: direct field write for properties with no setter contract.
		if (!bWroteValue)
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, NewObject);
		}
	}
	else
	{
		const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Template, PPF_None);
		if (!ImportResult)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' to value '%s' — check format"), *PropName, *Value));
		}
	}

	// Fire property-change notifications so setter-driven side effects run
	// (e.g. SkeletalMeshComponent mirrors SkinnedAsset ↔ SkeletalMesh and
	// updates render state). Without this, ObjectPtr properties can end up
	// set in memory but never serialized to the .uasset.
	{
		FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
		Template->PostEditChangeProperty(ChangeEvent);
	}

	// What UE actually stored, read before any compile can move the object.
	FString PostImportValue;
	Prop->ExportText_Direct(PostImportValue, ValuePtr, ValuePtr, Template, PPF_None);
	const FString ResolvedPropName = Prop->GetName();

	// PERSISTENCE. A write to an INHERITED NATIVE component lands on the CDO default subobject,
	// and a write to an Inheritable Component Handler override lands on an ICH record; neither is
	// re-serialised by MarkBlueprintAsModified alone — both silently revert on the next
	// reload/recompile. They persist only if the Blueprint is structurally modified AND
	// recompiled. SCS-template writes keep the lighter handshake.
	using EComponentSource = MonolithBlueprintComponentResolver::ESource;
	const bool bNeedsStructuralRecompile =
		Resolved.Source == EComponentSource::CdoNative || Resolved.Source == EComponentSource::IchOverride;

	bool bPersisted = true;
	FString PersistNote;

	if (bNeedsStructuralRecompile)
	{
		BP->Modify();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);

		// P1/P2. CompileBlueprint runs ValidateTemplates, which PRUNES any ICH record identical
		// to its archetype and renames the pruned template into the transient package — so
		// `Template` and `ValuePtr` may now dangle, and a value equal to the parent's is a silent
		// no-op. Re-resolve read-only and report the truth rather than echoing a stale pointer.
		const MonolithBlueprintComponentResolver::FResult After =
			MonolithBlueprintComponentResolver::Resolve(
				BP, CompName, UActorComponent::StaticClass(), /*bCreateIchOverride=*/false);

		if (After.IsValid())
		{
			if (FProperty* AfterProp = MonolithFindComponentProperty(After.Template->GetClass(), PropName))
			{
				const void* AfterPtr = AfterProp->ContainerPtrToValuePtr<void>(After.Template);
				FString AfterValue;
				AfterProp->ExportText_Direct(AfterValue, AfterPtr, AfterPtr, After.Template, PPF_None);
				bPersisted = AfterValue.Equals(PostImportValue);
				PostImportValue = AfterValue;
			}
			if (Resolved.Source == EComponentSource::IchOverride &&
				After.Source != EComponentSource::IchOverride)
			{
				bPersisted = false;
				PersistNote = TEXT("The value matches the inherited default, so the override record was pruned "
									"on compile and the component still reports the parent's value.");
			}
		}
		else
		{
			bPersisted = false;
			PersistNote = After.Error;
		}
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
	BP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), CompName);
	if (!Resolved.ResolvedName.IsNone())
	{
		Root->SetStringField(TEXT("resolved_component"), Resolved.ResolvedName.ToString());
	}
	Root->SetStringField(TEXT("source"), MonolithBlueprintComponentResolver::SourceToString(Resolved.Source));
	Root->SetStringField(TEXT("property"), ResolvedPropName);
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), PostImportValue);
	Root->SetBoolField(TEXT("persisted"), bPersisted);
	if (!PersistNote.IsEmpty())
	{
		Root->SetStringField(TEXT("note"), PersistNote);
	}
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// duplicate_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleDuplicateComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName = Params->GetStringField(TEXT("component_name"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));

	USCS_Node* SourceNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!SourceNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	if (!SourceNode->ComponentClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' has no ComponentClass"), *CompName));
	}

	// Build new node name
	FString NewName = Params->GetStringField(TEXT("new_name"));
	if (NewName.IsEmpty())
	{
		NewName = CompName + TEXT("_Copy");
	}

	// Ensure name is unique
	{
		FString BaseName = NewName;
		int32 Suffix = 1;
		while (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewName)))
		{
			NewName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
	}

	// Create a new SCS node for the same class
	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(SourceNode->ComponentClass, FName(*NewName));
	if (!NewNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create duplicate SCS node"));
	}

	// Duplicate the component template properties from source to new node
	if (SourceNode->ComponentTemplate && NewNode->ComponentTemplate)
	{
		UEngine::CopyPropertiesForUnrelatedObjects(SourceNode->ComponentTemplate, NewNode->ComponentTemplate);
	}

	// Preserve attach socket
	NewNode->AttachToName = SourceNode->AttachToName;

	// Attach to the same parent as the source
	USCS_Node* ParentNode = FindParentNode(BP->SimpleConstructionScript, SourceNode);
	FString ActualParentName;
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
		ActualParentName = ParentNode->GetVariableName().ToString();
	}
	else
	{
		BP->SimpleConstructionScript->AddNode(NewNode);
		ActualParentName = TEXT("");
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source"), CompName);
	Root->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Root->SetStringField(TEXT("class"), SourceNode->ComponentClass->GetName());
	Root->SetStringField(TEXT("parent"), ActualParentName);
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Shared helpers: per-property diff / apply for component class swaps and
// per-instance value dump / restore
// ---------------------------------------------------------------------------

namespace
{
	/** One property value as export text. */
	struct FBpComponentSwapPropValue
	{
		FString Property;
		FString Value;
	};

	/** One property that differed but was not captured, with the reason. */
	struct FBpComponentSwapPropSkip
	{
		FString Property;
		FString Reason;
	};

	/**
	 * Properties ignored outright: they are runtime state or not user-authored, so reporting them is noise.
	 * Returns true when the property must not be dumped, compared or restored at all.
	 */
	bool BpComponentSwapIsSilentlyIgnored(const FProperty* Prop)
	{
		return Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated)
			|| !Prop->HasAnyPropertyFlags(CPF_Edit);
	}

	/**
	 * Editable properties that cannot be round-tripped through export text on an instance.
	 * Returns the reason string, or nullptr when the property is capturable.
	 */
	const TCHAR* BpComponentSwapInstanceSkipReason(const FProperty* Prop)
	{
		if (Prop->HasAnyPropertyFlags(CPF_EditConst))
		{
			return TEXT("edit_const");
		}
		if (Prop->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
		{
			return TEXT("disable_edit_on_instance");
		}
		if (Prop->ContainsInstancedObjectProperty())
		{
			// The export text names a subobject owned by THIS component; it would dangle once the component is recreated.
			return TEXT("instanced_subobject");
		}
		if (Prop->ArrayDim != 1)
		{
			return TEXT("static_array");
		}
		return nullptr;
	}

	/** Full (non-delta) export text of one property on a container object. */
	FString BpComponentSwapExportProperty(const FProperty* Prop, UObject* Container)
	{
		FString Out;
		Prop->ExportText_InContainer(0, Out, Container, nullptr, Container, PPF_None);
		return Out;
	}

	/**
	 * Collect every editable property on Obj whose export text differs from Base's.
	 * Base falls back to Obj's class default object when null or of an unrelated class.
	 * bInstanceContext applies the instance-only skip rules (EditConst, DisableEditOnInstance, ...).
	 */
	void BpComponentSwapDiffProperties(UObject* Obj, UObject* Base, bool bInstanceContext,
		TArray<FBpComponentSwapPropValue>& OutValues, TArray<FBpComponentSwapPropSkip>& OutSkipped)
	{
		if (!Obj)
		{
			return;
		}
		UClass* ObjClass = Obj->GetClass();
		if (!Base || !Base->GetClass()->IsChildOf(ObjClass))
		{
			// Base must carry every property of Obj's class for the comparison to be meaningful.
			Base = ObjClass->GetDefaultObject();
		}

		for (TFieldIterator<FProperty> It(ObjClass); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop || BpComponentSwapIsSilentlyIgnored(Prop))
			{
				continue;
			}
			if (Prop->ArrayDim != 1)
			{
				// Element-0 export is not the whole property; compare element by element before reporting.
				bool bDiffers = false;
				for (int32 Index = 0; Index < Prop->ArrayDim && !bDiffers; ++Index)
				{
					bDiffers = !Prop->Identical_InContainer(Obj, Base, Index, PPF_None);
				}
				if (bDiffers)
				{
					OutSkipped.Add({ Prop->GetName(), TEXT("static_array") });
				}
				continue;
			}

			const FString ObjText = BpComponentSwapExportProperty(Prop, Obj);
			const FString BaseText = BpComponentSwapExportProperty(Prop, Base);
			if (ObjText.Equals(BaseText, ESearchCase::CaseSensitive))
			{
				continue;
			}

			if (bInstanceContext)
			{
				if (const TCHAR* Reason = BpComponentSwapInstanceSkipReason(Prop))
				{
					OutSkipped.Add({ Prop->GetName(), Reason });
					continue;
				}
			}
			else if (Prop->ContainsInstancedObjectProperty())
			{
				OutSkipped.Add({ Prop->GetName(), TEXT("instanced_subobject") });
				continue;
			}

			OutValues.Add({ Prop->GetName(), ObjText });
		}
	}

	/** Result of applying captured values to a live component. */
	struct FBpComponentSwapApplyReport
	{
		TArray<FString> Applied;
		TArray<FBpComponentSwapPropSkip> Skipped;
		TArray<FBpComponentSwapPropSkip> Failed;
		/** Name of the live component the values were verified on (after any construction-script rerun). */
		FString ComponentName;
		/** True when the final edit notification replaced the component (construction script rerun). */
		bool bComponentReplaced = false;
	};

	// Defined further down in this namespace.
	UActorComponent* BpComponentSwapFindActorComponent(AActor* Actor, const FString& Name);

	/**
	 * Find the live counterpart of a component after its owner's construction script reran.
	 * Blueprint-created components are destroyed and recreated by a rerun (the old object is renamed TRASH_*),
	 * so the old pointer must never be used again. Matches by name first, then by a unique archetype match.
	 */
	UActorComponent* BpComponentSwapReresolveComponent(AActor* Actor, const FString& Name, const UObject* Archetype)
	{
		if (!IsValid(Actor))
		{
			return nullptr;
		}
		UActorComponent* ByName = BpComponentSwapFindActorComponent(Actor, Name);
		if (IsValid(ByName))
		{
			return ByName;
		}
		if (!Archetype)
		{
			return nullptr;
		}
		UActorComponent* ByArchetype = nullptr;
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Candidate : Components)
		{
			if (IsValid(Candidate) && Candidate->GetArchetype() == Archetype)
			{
				if (ByArchetype)
				{
					return nullptr; // Ambiguous: refuse to guess.
				}
				ByArchetype = Candidate;
			}
		}
		return ByArchetype;
	}

	/**
	 * Import captured values onto an instance component and verify they persisted.
	 *
	 * A ValueSet PostEditChange on a registered component reruns the owner's construction script
	 * (UActorComponent::ConsolidatedPostEditChange), which replaces every Blueprint-created component. Per-property
	 * ValueSet notifications therefore wrote every value after the first onto a destroyed component. Instead, every
	 * value is imported under an Interactive notification (reregisters the component, no rerun), and a single ValueSet
	 * notification at the end performs the one rerun. The rerun carries the edits over to the new component through
	 * the component instance data cache (FActorComponentInstanceData serializes every editable property that differs
	 * from the archetype). The component is then re-resolved and each value re-exported from the live object, so
	 * Applied only lists values that are actually present afterwards.
	 */
	void BpComponentSwapApplyValues(AActor* Actor, UActorComponent* Comp, const TArray<FBpComponentSwapPropValue>& Values,
		FBpComponentSwapApplyReport& OutReport)
	{
		if (!Comp)
		{
			return;
		}
		const FString ComponentName = Comp->GetName();
		const UObject* Archetype = Comp->GetArchetype();
		OutReport.ComponentName = ComponentName;
		Comp->Modify();

		struct FImported
		{
			FString Property;
			FString ExpectedText;
		};
		TArray<FImported> Imported;
		FProperty* LastImportedProp = nullptr;

		for (const FBpComponentSwapPropValue& Entry : Values)
		{
			FProperty* Prop = MonolithFindComponentProperty(Comp->GetClass(), Entry.Property);
			if (!Prop)
			{
				OutReport.Skipped.Add({ Entry.Property, FString::Printf(TEXT("not_on_class (%s)"), *Comp->GetClass()->GetName()) });
				continue;
			}
			if (BpComponentSwapIsSilentlyIgnored(Prop))
			{
				OutReport.Skipped.Add({ Entry.Property, TEXT("transient_or_not_editable") });
				continue;
			}
			if (const TCHAR* Reason = BpComponentSwapInstanceSkipReason(Prop))
			{
				OutReport.Skipped.Add({ Entry.Property, Reason });
				continue;
			}

			// Interactive: the component is reregistered but the construction script is not rerun, so Comp stays live.
			Comp->PreEditChange(Prop);
			const TCHAR* ImportResult = Prop->ImportText_InContainer(*Entry.Value, Comp, Comp, PPF_None);
			FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::Interactive);
			Comp->PostEditChangeProperty(ChangeEvent);

			if (!ImportResult)
			{
				OutReport.Failed.Add({ Prop->GetName(), FString::Printf(TEXT("import_failed: '%s'"), *Entry.Value) });
				continue;
			}
			// Canonical form of what landed, so the post-rerun check is not fooled by input formatting.
			Imported.Add({ Prop->GetName(), BpComponentSwapExportProperty(Prop, Comp) });
			LastImportedProp = Prop;
		}

		if (Imported.Num() == 0)
		{
			return;
		}

		// One committing notification: reruns the construction script once, exactly as a details-panel edit does.
		Comp->PreEditChange(LastImportedProp);
		FPropertyChangedEvent FinalEvent(LastImportedProp, EPropertyChangeType::ValueSet);
		Comp->PostEditChangeProperty(FinalEvent);
		// Comp may be destroyed from here on.

		UActorComponent* Live = BpComponentSwapReresolveComponent(Actor, ComponentName, Archetype);
		if (!Live)
		{
			for (const FImported& Item : Imported)
			{
				OutReport.Failed.Add({ Item.Property, TEXT("component not found after the construction script rerun") });
			}
			return;
		}
		OutReport.bComponentReplaced = Live != Comp;
		OutReport.ComponentName = Live->GetName();

		for (const FImported& Item : Imported)
		{
			FProperty* LiveProp = MonolithFindComponentProperty(Live->GetClass(), Item.Property);
			if (!LiveProp)
			{
				OutReport.Failed.Add({ Item.Property, FString::Printf(TEXT("not_on_class after rerun (%s)"), *Live->GetClass()->GetName()) });
				continue;
			}
			const FString LiveText = BpComponentSwapExportProperty(LiveProp, Live);
			if (!LiveText.Equals(Item.ExpectedText, ESearchCase::CaseSensitive))
			{
				// Typical cause: the construction script graph sets this property, so the instance value is discarded.
				OutReport.Failed.Add({ Item.Property, FString::Printf(TEXT("did_not_persist: expected '%s', live value '%s'"),
					*Item.ExpectedText, *LiveText) });
				continue;
			}
			OutReport.Applied.Add(Item.Property);
		}
	}

	TSharedPtr<FJsonObject> BpComponentSwapValuesToJson(const TArray<FBpComponentSwapPropValue>& Values)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const FBpComponentSwapPropValue& V : Values)
		{
			Obj->SetStringField(V.Property, V.Value);
		}
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> BpComponentSwapSkipsToJson(const TArray<FBpComponentSwapPropSkip>& Skips, const FString& Component = FString())
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FBpComponentSwapPropSkip& S : Skips)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (!Component.IsEmpty())
			{
				Obj->SetStringField(TEXT("component"), Component);
			}
			Obj->SetStringField(TEXT("property"), S.Property);
			Obj->SetStringField(TEXT("reason"), S.Reason);
			Arr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Arr;
	}

	TSharedPtr<FJsonValue> BpComponentSwapOneSkipJson(const FString& Component, const FString& Property, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("component"), Component);
		Obj->SetStringField(TEXT("property"), Property);
		Obj->SetStringField(TEXT("reason"), Reason);
		return MakeShared<FJsonValueObject>(Obj);
	}

	/** Roots objects for the lifetime of the scope (only those that were not already rooted). */
	struct FBpComponentSwapKeepAlive
	{
		TArray<UObject*> Rooted;

		void Add(UObject* Obj)
		{
			if (Obj && !Obj->IsRooted())
			{
				Obj->AddToRoot();
				Rooted.Add(Obj);
			}
		}

		~FBpComponentSwapKeepAlive()
		{
			for (UObject* Obj : Rooted)
			{
				Obj->RemoveFromRoot();
			}
		}
	};

	TArray<TSharedPtr<FJsonValue>> BpComponentSwapStringsToJson(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : Strings)
		{
			Arr.Add(MakeShared<FJsonValueString>(S));
		}
		return Arr;
	}

	const TCHAR* BpComponentSwapCreationMethodToString(EComponentCreationMethod Method)
	{
		switch (Method)
		{
		case EComponentCreationMethod::Native:                   return TEXT("native");
		case EComponentCreationMethod::SimpleConstructionScript: return TEXT("scs");
		case EComponentCreationMethod::UserConstructionScript:   return TEXT("user_construction_script");
		case EComponentCreationMethod::Instance:                 return TEXT("instance");
		default:                                                 return TEXT("unknown");
		}
	}

	/** Editor (non-PIE) world, or null. */
	UWorld* BpComponentSwapEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	bool BpComponentSwapIsPieRunning()
	{
		return GEditor && GEditor->PlayWorld != nullptr;
	}

	/** Resolve a placed actor by full object path, then object name, then label. Errors on ambiguity. */
	AActor* BpComponentSwapFindLevelActor(const FString& InRef, FString& OutError)
	{
		const FString Ref = InRef.TrimStartAndEnd();
		if (Ref.IsEmpty())
		{
			OutError = TEXT("actor is required");
			return nullptr;
		}

		UWorld* World = BpComponentSwapEditorWorld();
		if (!World)
		{
			OutError = TEXT("No editor world is open");
			return nullptr;
		}

		if (Ref.StartsWith(TEXT("/")))
		{
			if (AActor* ByPath = FindObject<AActor>(nullptr, *Ref))
			{
				if (ByPath->GetWorld() == World && !ByPath->IsTemplate())
				{
					return ByPath;
				}
			}
		}

		TArray<AActor*> NameMatches;
		TArray<AActor*> LabelMatches;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsTemplate())
			{
				continue;
			}
			if (Actor->GetName().Equals(Ref, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(Ref, ESearchCase::IgnoreCase))
			{
				NameMatches.Add(Actor);
			}
			else if (Actor->GetActorLabel(false).Equals(Ref, ESearchCase::IgnoreCase))
			{
				LabelMatches.Add(Actor);
			}
		}

		const TArray<AActor*>& Matches = NameMatches.Num() > 0 ? NameMatches : LabelMatches;
		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Actor '%s' not found in the editor world (match by label, object name or full path)"), *Ref);
			return nullptr;
		}

		TArray<FString> Names;
		for (int32 Index = 0; Index < Matches.Num() && Index < 10; ++Index)
		{
			Names.Add(Matches[Index]->GetPathName());
		}
		OutError = FString::Printf(TEXT("Actor '%s' is ambiguous (%d matches); pass the full object path. Candidates: %s"),
			*Ref, Matches.Num(), *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UActorComponent* BpComponentSwapFindActorComponent(AActor* Actor, const FString& Name)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp && Comp->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	FString BpComponentSwapListComponentNames(AActor* Actor)
	{
		TArray<FString> Names;
		if (Actor)
		{
			TInlineComponentArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Comp : Components)
			{
				if (Comp)
				{
					Names.Add(Comp->GetName());
				}
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	/** Accept a JSON object param that the MCP transport may have serialised to a string. */
	TSharedPtr<FJsonObject> BpComponentSwapGetObjectParam(const TSharedPtr<FJsonObject>& Params, const FString& Name)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (Params->TryGetObjectField(Name, ObjPtr) && ObjPtr && ObjPtr->IsValid())
		{
			return *ObjPtr;
		}
		FString Text;
		if (Params->TryGetStringField(Name, Text) && !Text.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				return Parsed;
			}
		}
		return nullptr;
	}

	/** Resolve a component class from a native name, a /Script path, a class path, or a Blueprint asset path. */
	UClass* BpComponentSwapResolveClass(const FString& InName, FString& OutError)
	{
		const FString Name = InName.TrimStartAndEnd();
		UClass* Found = nullptr;

		if (Name.StartsWith(TEXT("/")))
		{
			Found = FindObject<UClass>(nullptr, *Name);
			if (!Found)
			{
				UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), Name);
				if (UBlueprint* LoadedBP = Cast<UBlueprint>(Loaded))
				{
					Found = LoadedBP->GeneratedClass.Get();
				}
				else
				{
					Found = Cast<UClass>(Loaded);
				}
			}
			if (!Found && !Name.StartsWith(TEXT("/Script/")))
			{
				Found = StaticLoadClass(UObject::StaticClass(), nullptr, *Name, {}, LOAD_NoWarn | LOAD_Quiet);
			}
		}
		else
		{
			Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
			if (!Found)
			{
				Found = FindFirstObject<UClass>(*(TEXT("U") + Name), EFindFirstObjectOptions::NativeFirst);
			}
		}

		if (!Found)
		{
			OutError = FString::Printf(TEXT("Component class not found: '%s'. Use a native class name, '/Script/Module.Class', or a Blueprint component asset path"), *Name);
			return nullptr;
		}

		// A Blueprint's skeleton class is not instantiable; redirect to the generated class.
		if (UBlueprint* OwnerBP = UBlueprint::GetBlueprintFromClass(Found))
		{
			if (OwnerBP->GeneratedClass.Get() && OwnerBP->GeneratedClass.Get() != Found)
			{
				Found = OwnerBP->GeneratedClass.Get();
			}
		}

		if (!Found->IsChildOf(UActorComponent::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Class '%s' is not a UActorComponent"), *Found->GetName());
			return nullptr;
		}
		if (Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("Class '%s' is abstract, deprecated or stale and cannot be used as a component template"), *Found->GetName());
			return nullptr;
		}
		return Found;
	}

	/** Blueprints whose generated class derives from BP's, loaded or not, nearest-to-BP first. */
	void BpComponentSwapCollectChildBlueprints(UBlueprint* BP, TArray<UBlueprint*>& OutChildren, TArray<FString>& OutLoadFailures)
	{
		UClass* GenClass = BP ? BP->GeneratedClass.Get() : nullptr;
		if (!GenClass)
		{
			return;
		}

		TSet<UBlueprint*> Seen;
		auto AddClass = [&](UClass* Class)
		{
			if (!Class || Class == GenClass || !Class->IsChildOf(GenClass) || Class->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				return;
			}
			UBlueprint* ChildBP = UBlueprint::GetBlueprintFromClass(Class);
			// GeneratedClass == Class filters out skeleton classes, which resolve to the same Blueprint.
			if (ChildBP && ChildBP != BP && ChildBP->GeneratedClass.Get() == Class && !Seen.Contains(ChildBP))
			{
				Seen.Add(ChildBP);
				OutChildren.Add(ChildBP);
			}
		};

		// Unloaded children: the asset registry answers from ParentClass tags.
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TSet<FTopLevelAssetPath> DerivedPaths;
		AR.GetDerivedClassNames({ GenClass->GetClassPathName() }, TSet<FTopLevelAssetPath>(), DerivedPaths);
		for (const FTopLevelAssetPath& Path : DerivedPaths)
		{
			const FString PathString = Path.ToString();
			UClass* Class = FindObject<UClass>(nullptr, *PathString);
			if (!Class)
			{
				Class = StaticLoadClass(UObject::StaticClass(), nullptr, *PathString, {}, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Class)
			{
				OutLoadFailures.Add(PathString);
				continue;
			}
			AddClass(Class);
		}

		// Loaded-but-unsaved children the registry may not know about yet.
		TArray<UClass*> LoadedDerived;
		GetDerivedClasses(GenClass, LoadedDerived, /*bRecursive=*/true);
		for (UClass* Class : LoadedDerived)
		{
			AddClass(Class);
		}

		// Parents before children: FindBestArchetype walks intermediate overrides, so an intermediate
		// Blueprint's override must be rebuilt before its own children's.
		auto Depth = [GenClass](const UBlueprint* Child)
		{
			int32 D = 0;
			for (const UClass* C = Child->GeneratedClass.Get(); C && C != GenClass; C = C->GetSuperClass())
			{
				++D;
			}
			return D;
		};
		OutChildren.StableSort([&Depth](const UBlueprint& A, const UBlueprint& B) { return Depth(&A) < Depth(&B); });
	}

	/** Compile through the module's compile action so the report shape matches compile_blueprint. */
	TSharedPtr<FJsonObject> BpComponentSwapCompile(UBlueprint* BP, bool& bOutSuccess)
	{
		TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
		CompileParams->SetStringField(TEXT("asset_path"), BP->GetPathName());
		const FMonolithActionResult Result = FMonolithBlueprintCompileActions::HandleCompileBlueprint(CompileParams);
		bOutSuccess = false;
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Result.Result->TryGetBoolField(TEXT("success"), bOutSuccess);
			return Result.Result;
		}
		TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Err->SetBoolField(TEXT("success"), false);
		Err->SetStringField(TEXT("error"), Result.ErrorMessage);
		return Err;
	}

	/** Move a template out of its outer so its name is free, the way ValidateTemplates discards ICH records. */
	void BpComponentSwapRetireTemplate(UObject* Template)
	{
		if (!Template)
		{
			return;
		}
		FLinkerLoad::InvalidateExport(Template);
		Template->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);
	}
}

// ---------------------------------------------------------------------------
// swap_component_class
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleSwapComponentClass(const TSharedPtr<FJsonObject>& Params)
{
	if (BpComponentSwapIsPieRunning())
	{
		return FMonolithActionResult::Error(TEXT("Cannot swap component classes while Play In Editor is running; stop PIE first"));
	}

	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}
	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	UClass* GenClass = BP->GeneratedClass.Get();
	if (!SCS || !GenClass)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript or generated class (is it a compiled Actor Blueprint?)"));
	}

	const FString CompName = Params->GetStringField(TEXT("component_name"));
	const FString NewClassName = Params->GetStringField(TEXT("new_class"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));
	if (NewClassName.IsEmpty()) return FMonolithActionResult::Error(TEXT("new_class is required"));

	bool bAllowUnrelated = false;
	bool bIncludeChildren = true;
	bool bPreserveInstances = false;
	Params->TryGetBoolField(TEXT("allow_unrelated"), bAllowUnrelated);
	Params->TryGetBoolField(TEXT("include_child_blueprints"), bIncludeChildren);
	Params->TryGetBoolField(TEXT("preserve_level_instances"), bPreserveInstances);

	USCS_Node* Node = FindSCSNodeByName(SCS, FName(*CompName));
	if (!Node)
	{
		TArray<USCS_Node*> Inherited;
		MonolithBlueprintComponentResolver::CollectInheritedScsNodes(BP, Inherited);
		for (USCS_Node* InheritedNode : Inherited)
		{
			if (InheritedNode && InheritedNode->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
			{
				UBlueprint* OwnerBP = InheritedNode->GetSCS() ? InheritedNode->GetSCS()->GetBlueprint() : nullptr;
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Component '%s' is declared on parent Blueprint '%s'. Call swap_component_class on that asset; "
						 "overrides in child Blueprints such as this one are migrated automatically"),
					*CompName, OwnerBP ? *OwnerBP->GetPathName() : TEXT("(unknown)")));
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Component '%s' is not a Blueprint-added (SCS) component of this Blueprint. Native C++ components "
				 "cannot be swapped by this action"), *CompName));
	}

	if (Node == SCS->GetDefaultSceneRootNode())
	{
		return FMonolithActionResult::Error(TEXT("The DefaultSceneRoot node is managed by the engine and cannot be swapped. Add a scene component and make it the root instead"));
	}

	UActorComponent* OldTemplate = Node->ComponentTemplate;
	if (!OldTemplate)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' has no template; compile the Blueprint and retry"), *CompName));
	}
	UClass* OldClass = OldTemplate->GetClass();

	FString ClassError;
	UClass* NewClass = BpComponentSwapResolveClass(NewClassName, ClassError);
	if (!NewClass)
	{
		return FMonolithActionResult::Error(ClassError);
	}
	if (NewClass == OldClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' is already a %s"), *CompName, *OldClass->GetName()));
	}

	const bool bOldScene = OldClass->IsChildOf(USceneComponent::StaticClass());
	const bool bNewScene = NewClass->IsChildOf(USceneComponent::StaticClass());
	if (bOldScene != bNewScene)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Cannot swap %s component '%s' to %s class '%s': scene and non-scene components occupy different places in "
				 "the hierarchy (attachment, transform, children). Remove and re-add the component instead"),
			bOldScene ? TEXT("scene") : TEXT("non-scene"), *CompName,
			bNewScene ? TEXT("scene") : TEXT("non-scene"), *NewClass->GetName()));
	}

	const bool bIsSubclass = NewClass->IsChildOf(OldClass);
	if (!bIsSubclass && !bAllowUnrelated)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not a subclass of '%s'. Pass allow_unrelated=true to swap anyway: only properties present on both "
				 "classes are copied, and graph nodes that call functions of the old class may fail to compile"),
			*NewClass->GetName(), *OldClass->GetName()));
	}

	const FName VariableName = Node->GetVariableName();
	const FGuid VariableGuid = Node->VariableGuid;
	const int32 ChildNodeCount = Node->GetChildNodes().Num();
	const FComponentKey Key(Node);

	TArray<FString> Warnings;

	// Retired templates are read after they leave their outer; keep them rooted until the action returns.
	FBpComponentSwapKeepAlive KeepAlive;

	// --- Child Blueprint overrides: snapshot BEFORE anything changes, while archetype lookups still
	//     resolve to the old class.
	struct FChildOverride
	{
		UBlueprint* ChildBP = nullptr;
		UInheritableComponentHandler* ICH = nullptr;
		UActorComponent* OldOverride = nullptr;
		UActorComponent* OldArchetype = nullptr;
		int32 OldOverrideDiffCount = 0;
	};
	TArray<FChildOverride> ChildOverrides;
	TArray<UBlueprint*> ChildBPs;
	if (bIncludeChildren)
	{
		TArray<FString> LoadFailures;
		BpComponentSwapCollectChildBlueprints(BP, ChildBPs, LoadFailures);
		for (const FString& Failed : LoadFailures)
		{
			Warnings.Add(FString::Printf(TEXT("Could not load derived class '%s'; its override (if any) was not migrated and will be dropped on its next compile"), *Failed));
		}
		for (UBlueprint* ChildBP : ChildBPs)
		{
			UInheritableComponentHandler* ICH = ChildBP->GetInheritableComponentHandler(false);
			if (!ICH)
			{
				continue;
			}
			UActorComponent* OldOverride = ICH->GetOverridenComponentTemplate(Key);
			if (!OldOverride)
			{
				continue;
			}
			FChildOverride& Entry = ChildOverrides.AddDefaulted_GetRef();
			Entry.ChildBP = ChildBP;
			Entry.ICH = ICH;
			Entry.OldOverride = OldOverride;
			Entry.OldArchetype = ICH->FindBestArchetype(Key);
			KeepAlive.Add(Entry.OldOverride);
			KeepAlive.Add(Entry.OldArchetype);
			TArray<FBpComponentSwapPropValue> Diff;
			TArray<FBpComponentSwapPropSkip> DiffSkipped;
			BpComponentSwapDiffProperties(OldOverride, Entry.OldArchetype, /*bInstanceContext=*/false, Diff, DiffSkipped);
			Entry.OldOverrideDiffCount = Diff.Num();
		}
	}

	// --- Level instances: snapshot per-instance values while archetypes still resolve to the old templates.
	struct FInstanceSnapshot
	{
		FString ActorPath;
		FString ComponentName;
		TArray<FBpComponentSwapPropValue> Values;
	};
	TArray<FInstanceSnapshot> InstanceSnapshots;
	if (bPreserveInstances)
	{
		if (UWorld* World = BpComponentSwapEditorWorld())
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (!Actor || Actor->IsTemplate() || !Actor->GetClass()->IsChildOf(GenClass))
				{
					continue;
				}
				UActorComponent* Comp = BpComponentSwapFindActorComponent(Actor, VariableName.ToString());
				if (!Comp || Comp->CreationMethod != EComponentCreationMethod::SimpleConstructionScript)
				{
					continue;
				}
				FInstanceSnapshot Snapshot;
				Snapshot.ActorPath = Actor->GetPathName();
				Snapshot.ComponentName = Comp->GetName();
				TArray<FBpComponentSwapPropSkip> Skipped;
				BpComponentSwapDiffProperties(Comp, Comp->GetArchetype(), /*bInstanceContext=*/true, Snapshot.Values, Skipped);
				if (Snapshot.Values.Num() > 0)
				{
					InstanceSnapshots.Add(MoveTemp(Snapshot));
				}
			}
		}
	}

	KeepAlive.Add(OldTemplate);
	UActorComponent* NewTemplate = nullptr;

	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintComponentActions", "SwapComponentClass", "Monolith Swap Component Class"));

		BP->Modify();
		SCS->Modify();
		Node->Modify();
		OldTemplate->Modify();

		// 1. Build the new template beside the old one (same outer, temporary name), then carry the
		//    designer's changes over. The delta is taken against the OLD class defaults, so values the
		//    designer never touched keep the new class's own defaults.
		const FString TemplateName = OldTemplate->GetName();
		const FName TempName = MakeUniqueObjectName(GenClass, NewClass, FName(*(TemplateName + TEXT("_SwapTmp"))));
		NewTemplate = NewObject<UActorComponent>(GenClass, NewClass, TempName, RF_ArchetypeObject | RF_Transactional | RF_Public);
		NewTemplate->Modify();
		{
			UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
			CopyParams.SourceObjectArchetype = OldClass->GetDefaultObject();
			CopyParams.bReplaceObjectClassReferences = false;
			CopyParams.bNotifyObjectReplacement = false;
			UEngine::CopyPropertiesForUnrelatedObjects(OldTemplate, NewTemplate, CopyParams);
		}

		// 2. Retire the old template and give the new one its name. Template and variable names must stay in
		//    sync (see USCS_Node::NameWasModified), and the node itself (GUID, children, attach data) is kept.
		BpComponentSwapRetireTemplate(OldTemplate);
		while (UObject* Collision = FindObject<UObject>(GenClass, *TemplateName))
		{
			Collision->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);
		}
		NewTemplate->Rename(*TemplateName, GenClass, REN_DoNotDirty | REN_DontCreateRedirectors);
		Node->ComponentClass = NewClass;
		Node->ComponentTemplate = NewTemplate;

		// 3. Rebuild child overrides against the new template, nearest child first. The engine would otherwise
		//    discard them on compile (IsRecordValid rejects a template whose class differs from the original).
		for (FChildOverride& Entry : ChildOverrides)
		{
			Entry.ChildBP->Modify();
			Entry.ICH->Modify();

			// Drop the record directly rather than through RemoveOverridenComponentTemplate, which marks the
			// template as garbage; the old override must stay readable for the copy below.
			for (auto RecordIt = Entry.ICH->CreateRecordIterator(); RecordIt; ++RecordIt)
			{
				if (RecordIt->ComponentKey.Match(Key))
				{
					RecordIt.RemoveCurrent();
					break;
				}
			}
			UActorComponent* OldOverride = Entry.OldOverride;
			BpComponentSwapRetireTemplate(OldOverride);

			UActorComponent* NewOverride = Entry.ICH->CreateOverridenComponentTemplate(Key);
			if (!NewOverride)
			{
				Warnings.Add(FString::Printf(TEXT("Could not create a new override in '%s'; its per-child values for '%s' were lost"),
					*Entry.ChildBP->GetPathName(), *VariableName.ToString()));
				continue;
			}
			NewOverride->SetFlags(RF_Transactional);
			NewOverride->Modify();

			UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
			CopyParams.SourceObjectArchetype = Entry.OldArchetype;
			CopyParams.bReplaceObjectClassReferences = false;
			CopyParams.bNotifyObjectReplacement = false;
			UEngine::CopyPropertiesForUnrelatedObjects(OldOverride, NewOverride, CopyParams);
		}

		// 4. Variable getters and other nodes typed by the component class pick up the new type.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		for (const FChildOverride& Entry : ChildOverrides)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Entry.ChildBP);
		}
	}

	// 5. Compile this Blueprint, then every child Blueprint (graphs there may reference the component too).
	bool bCompileOk = false;
	TSharedPtr<FJsonObject> CompileJson = BpComponentSwapCompile(BP, bCompileOk);
	BP->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> ChildReports;
	bool bChildrenCompileOk = true;
	for (UBlueprint* ChildBP : ChildBPs)
	{
		const FChildOverride* Entry = ChildOverrides.FindByPredicate([ChildBP](const FChildOverride& E) { return E.ChildBP == ChildBP; });

		FBlueprintEditorUtils::RefreshAllNodes(ChildBP);
		bool bChildOk = false;
		TSharedPtr<FJsonObject> ChildCompile = BpComponentSwapCompile(ChildBP, bChildOk);
		bChildrenCompileOk &= bChildOk;
		ChildBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("blueprint"), ChildBP->GetPathName());
		Report->SetBoolField(TEXT("had_override"), Entry != nullptr);
		if (Entry)
		{
			UInheritableComponentHandler* ICH = ChildBP->GetInheritableComponentHandler(false);
			UActorComponent* AfterOverride = ICH ? ICH->GetOverridenComponentTemplate(Key) : nullptr;
			if (AfterOverride)
			{
				TArray<FBpComponentSwapPropValue> Diff;
				TArray<FBpComponentSwapPropSkip> DiffSkipped;
				BpComponentSwapDiffProperties(AfterOverride, ICH->FindBestArchetype(Key), /*bInstanceContext=*/false, Diff, DiffSkipped);
				TArray<FString> Names;
				for (const FBpComponentSwapPropValue& V : Diff) { Names.Add(V.Property); }
				Report->SetStringField(TEXT("override_status"), AfterOverride->GetClass() == NewClass ? TEXT("migrated") : TEXT("class_mismatch"));
				Report->SetArrayField(TEXT("override_properties"), BpComponentSwapStringsToJson(Names));
			}
			else
			{
				// The rebuilt override matched the new parent template exactly, so compile pruned it: nothing lost.
				Report->SetStringField(TEXT("override_status"), TEXT("pruned_identical_to_parent"));
			}
			Report->SetNumberField(TEXT("override_properties_before"), Entry->OldOverrideDiffCount);
		}
		Report->SetBoolField(TEXT("compiled"), bChildOk);
		Report->SetObjectField(TEXT("compile"), ChildCompile);
		ChildReports.Add(MakeShared<FJsonValueObject>(Report));
	}

	// 6. Re-apply per-instance values on placed actors (reinstancing kept the actor paths).
	TArray<TSharedPtr<FJsonValue>> InstanceReports;
	if (InstanceSnapshots.Num() > 0)
	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintComponentActions", "SwapComponentRestoreInstances", "Monolith Restore Instance Component Values"));
		for (const FInstanceSnapshot& Snapshot : InstanceSnapshots)
		{
			TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
			Report->SetStringField(TEXT("actor"), Snapshot.ActorPath);
			AActor* Actor = FindObject<AActor>(nullptr, *Snapshot.ActorPath);
			UActorComponent* Comp = BpComponentSwapFindActorComponent(Actor, Snapshot.ComponentName);
			if (!Comp)
			{
				Report->SetStringField(TEXT("error"), TEXT("actor or component not found after reinstancing"));
				Report->SetObjectField(TEXT("values"), BpComponentSwapValuesToJson(Snapshot.Values));
			}
			else
			{
				Actor->Modify();
				FBpComponentSwapApplyReport Applied;
				BpComponentSwapApplyValues(Actor, Comp, Snapshot.Values, Applied);
				Actor->MarkPackageDirty();
				Report->SetStringField(TEXT("component"), Applied.ComponentName);
				Report->SetArrayField(TEXT("applied"), BpComponentSwapStringsToJson(Applied.Applied));
				Report->SetArrayField(TEXT("skipped"), BpComponentSwapSkipsToJson(Applied.Skipped));
				Report->SetArrayField(TEXT("failed"), BpComponentSwapSkipsToJson(Applied.Failed));
			}
			InstanceReports.Add(MakeShared<FJsonValueObject>(Report));
		}
	}

	// 7. Report what the node looks like now.
	USCS_Node* AfterNode = FindSCSNodeByName(SCS, VariableName);
	TArray<FBpComponentSwapPropValue> TemplateDiff;
	TArray<FBpComponentSwapPropSkip> TemplateSkipped;
	if (AfterNode && AfterNode->ComponentTemplate)
	{
		BpComponentSwapDiffProperties(AfterNode->ComponentTemplate, NewClass->GetDefaultObject(), /*bInstanceContext=*/false, TemplateDiff, TemplateSkipped);
	}
	TArray<FString> TemplateDiffNames;
	for (const FBpComponentSwapPropValue& V : TemplateDiff) { TemplateDiffNames.Add(V.Property); }

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("component"), VariableName.ToString());
	Root->SetStringField(TEXT("old_class"), OldClass->GetPathName());
	Root->SetStringField(TEXT("new_class"), NewClass->GetPathName());
	Root->SetBoolField(TEXT("subclass_swap"), bIsSubclass);
	Root->SetStringField(TEXT("variable_guid"), VariableGuid.ToString());
	Root->SetBoolField(TEXT("guid_preserved"), AfterNode && AfterNode->VariableGuid == VariableGuid);
	Root->SetBoolField(TEXT("class_applied"), AfterNode && AfterNode->ComponentClass == NewClass
		&& AfterNode->ComponentTemplate && AfterNode->ComponentTemplate->GetClass() == NewClass);
	Root->SetNumberField(TEXT("child_nodes"), AfterNode ? AfterNode->GetChildNodes().Num() : 0);
	Root->SetNumberField(TEXT("child_nodes_before"), ChildNodeCount);
	Root->SetArrayField(TEXT("template_non_default_properties"), BpComponentSwapStringsToJson(TemplateDiffNames));
	Root->SetArrayField(TEXT("template_skipped"), BpComponentSwapSkipsToJson(TemplateSkipped));
	Root->SetBoolField(TEXT("compiled"), bCompileOk);
	Root->SetObjectField(TEXT("compile"), CompileJson);
	Root->SetNumberField(TEXT("child_blueprints_scanned"), ChildBPs.Num());
	Root->SetNumberField(TEXT("child_overrides_migrated"), ChildOverrides.Num());
	Root->SetBoolField(TEXT("child_blueprints_compiled"), bChildrenCompileOk);
	Root->SetArrayField(TEXT("child_blueprints"), ChildReports);
	if (bPreserveInstances)
	{
		Root->SetArrayField(TEXT("level_instances"), InstanceReports);
	}
	Root->SetArrayField(TEXT("warnings"), BpComponentSwapStringsToJson(Warnings));
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// dump_instance_component_values
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleDumpInstanceComponentValues(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	AActor* Actor = BpComponentSwapFindLevelActor(Params->GetStringField(TEXT("actor")), Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString Filter = Params->GetStringField(TEXT("component_name"));
	TInlineComponentArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	if (!Filter.IsEmpty() && !BpComponentSwapFindActorComponent(Actor, Filter))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' not found on '%s'. Components: %s"),
			*Filter, *Actor->GetActorLabel(false), *BpComponentSwapListComponentNames(Actor)));
	}

	TSharedPtr<FJsonObject> ValuesJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ComponentReports;
	int32 TotalValues = 0;
	int32 Scanned = 0;

	for (UActorComponent* Comp : Components)
	{
		if (!Comp || (!Filter.IsEmpty() && !Comp->GetName().Equals(Filter, ESearchCase::IgnoreCase)))
		{
			continue;
		}
		++Scanned;

		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("name"), Comp->GetName());
		Report->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
		Report->SetStringField(TEXT("creation_method"), BpComponentSwapCreationMethodToString(Comp->CreationMethod));

		if (Comp->CreationMethod == EComponentCreationMethod::UserConstructionScript)
		{
			// Recreated by the construction script on every rerun; per-instance edits do not persist.
			Report->SetStringField(TEXT("note"), TEXT("created by the construction script graph; per-instance values are not persistent and were not dumped"));
			Report->SetNumberField(TEXT("value_count"), 0);
			ComponentReports.Add(MakeShared<FJsonValueObject>(Report));
			continue;
		}

		UObject* Archetype = Comp->GetArchetype();
		TArray<FBpComponentSwapPropValue> Values;
		TArray<FBpComponentSwapPropSkip> Skipped;
		BpComponentSwapDiffProperties(Comp, Archetype, /*bInstanceContext=*/true, Values, Skipped);

		Report->SetStringField(TEXT("archetype"), Archetype ? Archetype->GetPathName() : TEXT(""));
		Report->SetNumberField(TEXT("value_count"), Values.Num());
		Report->SetObjectField(TEXT("values"), BpComponentSwapValuesToJson(Values));
		Report->SetArrayField(TEXT("skipped"), BpComponentSwapSkipsToJson(Skipped));

		if (Values.Num() > 0 || !Filter.IsEmpty())
		{
			ValuesJson->SetObjectField(Comp->GetName(), BpComponentSwapValuesToJson(Values));
		}
		TotalValues += Values.Num();

		if (Values.Num() > 0 || Skipped.Num() > 0 || !Filter.IsEmpty())
		{
			ComponentReports.Add(MakeShared<FJsonValueObject>(Report));
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("actor"), Actor->GetActorLabel(false));
	Root->SetStringField(TEXT("actor_name"), Actor->GetName());
	Root->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Root->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetPathName());
	Root->SetNumberField(TEXT("components_scanned"), Scanned);
	Root->SetNumberField(TEXT("value_count"), TotalValues);
	Root->SetArrayField(TEXT("components"), ComponentReports);
	Root->SetObjectField(TEXT("values"), ValuesJson);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// restore_instance_component_values
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleRestoreInstanceComponentValues(const TSharedPtr<FJsonObject>& Params)
{
	if (BpComponentSwapIsPieRunning())
	{
		return FMonolithActionResult::Error(TEXT("Cannot restore instance values while Play In Editor is running; stop PIE first"));
	}

	FString Error;
	AActor* Actor = BpComponentSwapFindLevelActor(Params->GetStringField(TEXT("actor")), Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> ValuesJson = BpComponentSwapGetObjectParam(Params, TEXT("values"));
	if (!ValuesJson.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("values is required: an object of { \"<component>\": { \"<property>\": \"<export text>\" } }"));
	}
	// Accept the whole dump_instance_component_values response as well as its 'values' field.
	{
		const TSharedPtr<FJsonObject>* Inner = nullptr;
		if (ValuesJson->HasField(TEXT("actor_path")) && ValuesJson->TryGetObjectField(TEXT("values"), Inner) && Inner && Inner->IsValid())
		{
			ValuesJson = *Inner;
		}
	}

	const FString Filter = Params->GetStringField(TEXT("component_name"));

	TArray<TSharedPtr<FJsonValue>> AppliedArr;
	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;

	FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprintComponentActions", "RestoreInstanceComponentValues", "Monolith Restore Instance Component Values"));
	Actor->Modify();

	for (const TPair<FString, TSharedPtr<FJsonValue>>& CompPair : ValuesJson->Values)
	{
		if (!Filter.IsEmpty() && !CompPair.Key.Equals(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (!CompPair.Value.IsValid() || !CompPair.Value->TryGetObject(PropsObj) || !PropsObj || !PropsObj->IsValid())
		{
			FailedArr.Add(BpComponentSwapOneSkipJson(CompPair.Key, TEXT("*"), TEXT("component entry is not an object of property -> text")));
			continue;
		}

		UActorComponent* Comp = BpComponentSwapFindActorComponent(Actor, CompPair.Key);
		if (!Comp)
		{
			FailedArr.Add(BpComponentSwapOneSkipJson(CompPair.Key, TEXT("*"), FString::Printf(TEXT("component not found; components: %s"),
				*BpComponentSwapListComponentNames(Actor))));
			continue;
		}

		TArray<FBpComponentSwapPropValue> Values;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& PropPair : (*PropsObj)->Values)
		{
			FString Text;
			if (!PropPair.Value.IsValid() || !PropPair.Value->TryGetString(Text))
			{
				FailedArr.Add(BpComponentSwapOneSkipJson(Comp->GetName(), PropPair.Key, TEXT("value must be export text (string)")));
				continue;
			}
			Values.Add({ PropPair.Key, Text });
		}

		FBpComponentSwapApplyReport Report;
		BpComponentSwapApplyValues(Actor, Comp, Values, Report);
		// Comp may have been replaced by a construction-script rerun; use the live component's name only.
		Comp = nullptr;
		for (const FString& Name : Report.Applied)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("component"), Report.ComponentName);
			Obj->SetStringField(TEXT("property"), Name);
			AppliedArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		SkippedArr.Append(BpComponentSwapSkipsToJson(Report.Skipped, Report.ComponentName));
		FailedArr.Append(BpComponentSwapSkipsToJson(Report.Failed, Report.ComponentName));
	}

	if (AppliedArr.Num() > 0)
	{
		Actor->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("actor"), Actor->GetActorLabel(false));
	Root->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Root->SetNumberField(TEXT("applied_count"), AppliedArr.Num());
	Root->SetNumberField(TEXT("skipped_count"), SkippedArr.Num());
	Root->SetNumberField(TEXT("failed_count"), FailedArr.Num());
	Root->SetArrayField(TEXT("applied"), AppliedArr);
	Root->SetArrayField(TEXT("skipped"), SkippedArr);
	Root->SetArrayField(TEXT("failed"), FailedArr);
	return FMonolithActionResult::Success(Root);
}
