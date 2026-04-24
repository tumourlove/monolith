#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithBlueprintComponentActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_component"),
		TEXT("Add a new component to a Blueprint's construction script. Returns variable_name, class, and parent."),
		FMonolithActionHandler::CreateStatic(&HandleAddComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("Component class name (e.g. 'StaticMeshComponent')"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Variable name for the new component"))
			.Optional(TEXT("parent"), TEXT("string"), TEXT("Parent component variable name (attach as child)"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_component"),
		TEXT("Remove a component from a Blueprint's construction script. Optionally promotes children to the removed node's parent."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to remove"))
			.Optional(TEXT("promote_children"), TEXT("boolean"), TEXT("Reparent children to removed node's parent (default: true)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_component"),
		TEXT("Rename a component variable in a Blueprint."),
		FMonolithActionHandler::CreateStatic(&HandleRenameComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Current component variable name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New variable name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reparent_component"),
		TEXT("Change the parent of a component in a Blueprint. Pass empty string for new_parent to make it a root component."),
		FMonolithActionHandler::CreateStatic(&HandleReparentComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to reparent"))
			.Required(TEXT("new_parent"), TEXT("string"), TEXT("New parent component variable name, or empty string to attach to root"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on new parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_component_property"),
		TEXT("Set a property on a component template in a Blueprint via text import."),
		FMonolithActionHandler::CreateStatic(&HandleSetComponentProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name on the component"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as text (same format as copy/paste in Details panel)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_component"),
		TEXT("Duplicate a component in a Blueprint. The copy is attached to the same parent as the original."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateComponent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Variable name for the duplicated component"))
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

	// 1) BP-added components live on the SimpleConstructionScript as USCS_Node.
	UActorComponent* Template = nullptr;
	if (BP->SimpleConstructionScript)
	{
		if (USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName)))
		{
			Template = Node->ComponentTemplate;
		}
	}

	// 2) Fallback: native inherited components (declared in the C++ parent class)
	// don't appear in the SCS — they live as default subobjects on the CDO.
	// Writing to the CDO subobject persists in the saved BP.
	if (!Template && BP->GeneratedClass)
	{
		if (UObject* CDO = BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
		{
			if (AActor* CDOActor = Cast<AActor>(CDO))
			{
				TArray<UActorComponent*> Comps;
				CDOActor->GetComponents(Comps);
				for (UActorComponent* Comp : Comps)
				{
					if (!Comp) continue;
					if (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase) ||
					    Comp->GetFName() == FName(*CompName))
					{
						Template = Comp;
						break;
					}
				}
			}
		}
	}

	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		// Try case-insensitive search
		for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}
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
			NewObject = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
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
		ObjProp->SetObjectPropertyValue(ValuePtr, NewObject);
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

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	BP->MarkPackageDirty();

	// Re-export to reflect whatever UE actually stored — caller can diff old vs new.
	FString PostImportValue;
	Prop->ExportText_Direct(PostImportValue, ValuePtr, ValuePtr, Template, PPF_None);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("property"), Prop->GetName());
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), PostImportValue);
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
