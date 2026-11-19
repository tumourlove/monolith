#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithPinTypeGrammar.h"
#include "MonolithParamSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintEditorLibrary.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Variable.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UObjectIterator.h"

// --- Registration ---

void FMonolithBlueprintGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_function"),
		TEXT("Add a new function graph to a Blueprint. Optionally seed its parameters from a prototype UFunction (prototype_class + prototype_function), which copies by-reference / const parameter flags exactly."),
		FMonolithActionHandler::CreateStatic(&HandleAddFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Function name"), {TEXT("function_name")})
			.Optional(TEXT("is_pure"), TEXT("bool"), TEXT("Mark as pure (no exec pins)"), TEXT("false"))
			.Optional(TEXT("is_const"), TEXT("bool"), TEXT("Mark as const"), TEXT("false"))
			.Optional(TEXT("is_static"), TEXT("bool"), TEXT("Mark as static"), TEXT("false"))
			.Optional(TEXT("call_in_editor"), TEXT("bool"), TEXT("Show 'Call In Editor' button"), TEXT("false"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Function category"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Function tooltip/description"))
			.Optional(TEXT("access"), TEXT("string"), TEXT("Access specifier: Public, Protected, or Private"), TEXT("Public"))
			.Optional(TEXT("replication"), TEXT("string"), TEXT("Replication mode: none, multicast, server, client (default: none)"))
			.Optional(TEXT("reliable"), TEXT("bool"), TEXT("Use reliable replication (default: false)"))
			.Optional(TEXT("prototype_class"), TEXT("string"), TEXT("Class declaring a prototype UFunction whose signature seeds the new function's entry/result pins (native name e.g. AnimExecutionContextLibrary, /Script/Module.Class path, or Blueprint asset path). Requires prototype_function."))
			.Optional(TEXT("prototype_function"), TEXT("string"), TEXT("Prototype function name on prototype_class (or a full /Script/Module.Class:Function path, in which case prototype_class may be omitted). Pins copy name, type, by-reference and const exactly. Errors if not found; never falls back to an empty signature."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_function_thread_safe"),
		TEXT("Set (or clear) the 'Thread Safe' flag on an existing Blueprint function graph. Sets FKismetUserDeclaredFunctionMetadata::bThreadSafe on the function's entry node and recompiles. Required for functions called from BlueprintThreadSafeUpdateAnimation. Searches function graphs (including AnimBP function graphs)."),
		FMonolithActionHandler::CreateStatic(&HandleSetFunctionThreadSafe),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"), {TEXT("name")})
			.Optional(TEXT("thread_safe"), TEXT("bool"), TEXT("Set the Thread Safe flag (true) or clear it (false)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("override_parent_function"),
		TEXT("Author a Blueprint override of an overridable parent function (BlueprintImplementableEvent / BlueprintNativeEvent), including those that RETURN a value (e.g. UCommonActivatableWidget::BP_GetDesiredFocusTarget -> UWidget*). add_function cannot do this and the event-node form has no ReturnValue pin. Declaring class is resolved generically by name. Returns graph_name, entry_node_id, return_pin_id/name, override_class, has_return_value."),
		FMonolithActionHandler::CreateStatic(&HandleOverrideParentFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("parent_function_name"), TEXT("string"), TEXT("Name of the overridable parent function"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_function"),
		TEXT("Remove a function graph from a Blueprint by name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Function name to remove"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_function_flags"),
		TEXT("Edit flags and metadata on an existing Blueprint function graph: pure, const, Call In Editor, access specifier, category, description. Only the fields you pass change. Mirrors the My Blueprint details panel (function entry node extra flags + metadata, plus the skeleton UFunction) and marks the Blueprint structurally modified. Returns the resulting is_pure / is_const / call_in_editor / access."),
		FMonolithActionHandler::CreateStatic(&HandleSetFunctionFlags),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"), {TEXT("name")})
			.Optional(TEXT("is_pure"), TEXT("bool"), TEXT("Set or clear BlueprintPure"))
			.Optional(TEXT("is_const"), TEXT("bool"), TEXT("Set or clear Const (rejected on static functions)"))
			.Optional(TEXT("call_in_editor"), TEXT("bool"), TEXT("Set or clear the 'Call In Editor' metadata"))
			.Optional(TEXT("access"), TEXT("string"), TEXT("Access specifier: Public, Protected, or Private"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Function category (empty string clears it)"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Function tooltip/description (empty string clears it)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_event_graph"),
		TEXT("Add a new Event Graph (ubergraph page) to a Blueprint, like 'Add New > Event Graph' in the editor. The new page uses the same schema as the Blueprint's existing event graph. Returns graph_name."),
		FMonolithActionHandler::CreateStatic(&HandleAddEventGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Event graph name (default: a unique 'NewEventGraph' name)"), {TEXT("graph_name")})
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_function"),
		TEXT("Rename an existing function graph in a Blueprint. Function flags (pure, const, access, ...) and metadata (Call In Editor, category, tooltip, thread safe) are preserved."),
		FMonolithActionHandler::CreateStatic(&HandleRenameFunction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current function name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New function name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_macro"),
		TEXT("Add a new macro graph to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Macro name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_macro"),
		TEXT("Remove a macro graph from a Blueprint by name"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("macro_name"), TEXT("string"), TEXT("Macro name to remove"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_macro"),
		TEXT("Rename an existing macro graph in a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRenameMacro),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current macro name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New macro name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_event_dispatcher"),
		TEXT("Add a new event dispatcher (multicast delegate) to a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleAddEventDispatcher),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Event dispatcher name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_function_params"),
		TEXT("Add input/output parameters to a Blueprint function (appends; use remove_function_param / reorder_function_params to remove or reorder). Inputs accept optional is_reference / is_const for by-reference and const-reference parameters. Call sites are refreshed so they show the new pins."),
		FMonolithActionHandler::CreateStatic(&HandleSetFunctionParams),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type, is_reference?, is_const?} objects for inputs. is_reference=true makes the parameter pass-by-reference; is_const=true (requires is_reference) makes it a const reference, e.g. 'const FAnimUpdateContext&'"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for outputs (is_reference / is_const are rejected on outputs)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_function_param"),
		TEXT("Remove one input or output parameter from a Blueprint function. Mirrors the details panel: entry/result nodes are rebuilt, call sites in every loaded Blueprint are reconstructed, and call-site pins left orphaned by the removal (including linked ones that would fail compilation) are dropped. Outputs are removed from every return node. Returns call_sites_cleaned and, for inputs, dangling_parameter_nodes (Get/Set nodes of the removed parameter inside the function) plus linked_dangling_parameter_nodes (the connected subset, which blocks compilation; unconnected ones are ignored by the compiler)."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveFunctionParam),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"))
			.Required(TEXT("param_name"), TEXT("string"), TEXT("Parameter name (case-insensitive)"), {TEXT("param")})
			.Optional(TEXT("direction"), TEXT("string"), TEXT("'input', 'output' or 'any' (inputs searched first)"), TEXT("any"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reorder_function_params"),
		TEXT("Reorder a Blueprint function's inputs or outputs. Names in 'order' move to the front in that order; unlisted parameters keep their relative order after them. Entry/result nodes and all call sites are rebuilt so the pin order matches. Returns the final order."),
		FMonolithActionHandler::CreateStatic(&HandleReorderFunctionParams),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("function_name"), TEXT("string"), TEXT("Function graph name"))
			.Required(TEXT("order"), TEXT("array"), TEXT("Parameter names (strings) in the desired order"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("'input' (default) or 'output'"), TEXT("input"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("implement_interface"),
		TEXT("Add an interface to a Blueprint's implemented interface list"),
		FMonolithActionHandler::CreateStatic(&HandleImplementInterface),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"), TEXT("string"), TEXT("Interface class name (e.g. IMyInterface)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_interface"),
		TEXT("Remove an interface from a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveInterface),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"), TEXT("string"), TEXT("Interface class name to remove"))
			.Optional(TEXT("preserve_functions"), TEXT("bool"), TEXT("Keep stub functions after removal"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reparent_blueprint"),
		TEXT("Change the parent class of a Blueprint"),
		FMonolithActionHandler::CreateStatic(&HandleReparentBlueprint),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("new_parent_class"), TEXT("string"), TEXT("New parent class name"))
			.Build());

	// ---- Wave 6 ----

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_event_dispatcher"),
		TEXT("Remove an event dispatcher (multicast delegate) from a Blueprint. Warns if any graph nodes still reference it."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEventDispatcher),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),      TEXT("Blueprint asset path"))
			.Required(TEXT("dispatcher_name"), TEXT("string"), TEXT("Event dispatcher name (without _Signature suffix)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_event_dispatcher_params"),
		TEXT("Set (replace) the signature parameters on an event dispatcher. Existing params are cleared and replaced with the new list."),
		FMonolithActionHandler::CreateStatic(&HandleSetEventDispatcherParams),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),      TEXT("Blueprint asset path"))
			.Required(TEXT("dispatcher_name"), TEXT("string"), TEXT("Event dispatcher name (without _Signature suffix)"))
			.Required(TEXT("params"),          TEXT("array"),  TEXT("Array of {name, type} objects for the new signature"))
			.Build());

	// ---- Wave 5 ----

	Registry.RegisterAction(TEXT("blueprint"), TEXT("scaffold_interface_implementation"),
		TEXT("Add an interface to a Blueprint AND create all stub function graphs in one call. Returns the interface name and list of created graphs. Much more useful than implement_interface alone — this one actually wires up the stubs."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldInterfaceImplementation),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"),       TEXT("Blueprint asset path"))
			.Required(TEXT("interface_class"),   TEXT("string"), TEXT("Interface class name (e.g. BPI_Interactable or IBpi_Interactable)"))
			.Build());
}

// --- add_function ---

namespace
{
	/** Resolves a class from a native name (with or without U/A prefix), a /Script/ path, or a Blueprint asset path. */
	UClass* MonolithGraphActionsResolvePrototypeClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}
		if (ClassName.Contains(TEXT("/")))
		{
			if (UClass* ByPath = FindObject<UClass>(nullptr, *ClassName))
			{
				return ByPath;
			}
			if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				return Loaded;
			}
			if (UBlueprint* ProtoBP = LoadObject<UBlueprint>(nullptr, *ClassName, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				return ProtoBP->GeneratedClass ? ProtoBP->GeneratedClass.Get() : ProtoBP->SkeletonGeneratedClass.Get();
			}
			return nullptr;
		}
		if (UClass* Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		// Accept C++-style prefixed names (UFoo / AFoo -> Foo).
		if (ClassName.Len() > 1 && (ClassName[0] == TEXT('U') || ClassName[0] == TEXT('A')) && FChar::IsUpper(ClassName[1]))
		{
			return FindFirstObject<UClass>(*ClassName.Mid(1), EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	UFunction* MonolithGraphActionsResolvePrototypeFunction(const FString& ClassName, const FString& FuncName, FString& OutError)
	{
		if (FuncName.IsEmpty())
		{
			OutError = TEXT("prototype_class was given without prototype_function");
			return nullptr;
		}

		// Full object path form: /Script/Module.Class:Function (or Class.Function).
		if (ClassName.IsEmpty())
		{
			if (!FuncName.Contains(TEXT("/")))
			{
				OutError = TEXT("prototype_function needs prototype_class, or must be a full path like /Script/Module.Class:Function");
				return nullptr;
			}
			UFunction* ByPath = FindObject<UFunction>(nullptr, *FuncName);
			if (!ByPath)
			{
				ByPath = FindObject<UFunction>(nullptr, *FuncName.Replace(TEXT(":"), TEXT(".")));
			}
			if (!ByPath)
			{
				OutError = FString::Printf(TEXT("Prototype function not found: %s"), *FuncName);
			}
			return ByPath;
		}

		UClass* ProtoClass = MonolithGraphActionsResolvePrototypeClass(ClassName);
		if (!ProtoClass)
		{
			OutError = FString::Printf(TEXT("Prototype class not found: %s"), *ClassName);
			return nullptr;
		}
		UFunction* Func = ProtoClass->FindFunctionByName(FName(*FuncName));
		if (!Func)
		{
			OutError = FString::Printf(TEXT("Prototype function '%s' not found on class %s"), *FuncName, *ProtoClass->GetPathName());
		}
		return Func;
	}
}

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName = Params->GetStringField(TEXT("name"));
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetName() == FuncName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Function already exists: %s"), *FuncName));
		}
	}

	// Optional signature prototype. Resolved BEFORE creating the graph so a bad
	// prototype leaves the Blueprint untouched; there is deliberately no fallback
	// to the unseeded path.
	FString PrototypeClassName, PrototypeFuncName;
	Params->TryGetStringField(TEXT("prototype_class"), PrototypeClassName);
	Params->TryGetStringField(TEXT("prototype_function"), PrototypeFuncName);
	PrototypeClassName.TrimStartAndEndInline();
	PrototypeFuncName.TrimStartAndEndInline();

	UFunction* Prototype = nullptr;
	if (!PrototypeClassName.IsEmpty() || !PrototypeFuncName.IsEmpty())
	{
		FString PrototypeError;
		Prototype = MonolithGraphActionsResolvePrototypeFunction(PrototypeClassName, PrototypeFuncName, PrototypeError);
		if (!Prototype)
		{
			return FMonolithActionResult::Error(PrototypeError);
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create function graph: %s"), *FuncName));
	}

	if (Prototype)
	{
		// UFunction overload = copy-signature: entry/result get user-defined pins
		// mirroring the prototype's parameters (incl. bIsReference / bIsConst).
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(BP, NewGraph, /*bIsUserCreated=*/true, Prototype);
	}
	else
	{
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
	}

	// Find the entry node to set metadata and flags
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (EntryNode)
	{
		uint32 ExtraFlags = EntryNode->GetFunctionFlags();

		bool bIsPure = false;
		bool bIsConst = false;
		bool bIsStatic = false;
		bool bCallInEditor = false;
		Params->TryGetBoolField(TEXT("is_pure"), bIsPure);
		Params->TryGetBoolField(TEXT("is_const"), bIsConst);
		Params->TryGetBoolField(TEXT("is_static"), bIsStatic);
		Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor);

		if (bIsPure)        ExtraFlags |= FUNC_BlueprintPure;
		if (bIsConst)       ExtraFlags |= FUNC_Const;
		if (bIsStatic)      ExtraFlags |= FUNC_Static;

		// Access specifier
		FString Access;
		Params->TryGetStringField(TEXT("access"), Access);
		ExtraFlags &= ~(FUNC_Protected | FUNC_Private); // clear existing
		if (Access == TEXT("Protected"))      ExtraFlags |= FUNC_Protected;
		else if (Access == TEXT("Private"))   ExtraFlags |= FUNC_Private;

		// RPC / Multicast replication flags (Phase 5A)
		FString Replication;
		if (Params->TryGetStringField(TEXT("replication"), Replication) && !Replication.IsEmpty() && Replication != TEXT("none"))
		{
			const uint32 FlagsToClear = FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient;
			ExtraFlags &= ~FlagsToClear;

			uint32 NetFlag = 0;
			FString Lower = Replication.ToLower();
			if (Lower == TEXT("multicast"))      NetFlag = FUNC_NetMulticast;
			else if (Lower == TEXT("server"))    NetFlag = FUNC_NetServer;
			else if (Lower == TEXT("client"))    NetFlag = FUNC_NetClient;

			if (NetFlag != 0)
				ExtraFlags |= (FUNC_Net | NetFlag);
		}

		bool bReliable = false;
		if (Params->TryGetBoolField(TEXT("reliable"), bReliable) && bReliable)
			ExtraFlags |= FUNC_NetReliable;

		EntryNode->SetExtraFlags(ExtraFlags);
		EntryNode->MetaData.bCallInEditor = bCallInEditor;

		FString Category;
		Params->TryGetStringField(TEXT("category"), Category);
		if (!Category.IsEmpty())
		{
			EntryNode->MetaData.Category = FText::FromString(Category);
		}

		FString Description;
		Params->TryGetStringField(TEXT("description"), Description);
		if (!Description.IsEmpty())
		{
			EntryNode->MetaData.ToolTip = FText::FromString(Description);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetNumberField(TEXT("node_count"), NewGraph->Nodes.Num());
	if (Prototype)
	{
		Root->SetStringField(TEXT("prototype"), Prototype->GetPathName());

		// Report the seeded parameters so callers can confirm ref/const flags.
		TArray<TSharedPtr<FJsonValue>> ParamArr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			const UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
			if (!Editable)
			{
				continue;
			}
			const bool bEntry = Node->IsA<UK2Node_FunctionEntry>();
			for (const TSharedPtr<FUserPinInfo>& PinInfo : Editable->UserDefinedPins)
			{
				if (!PinInfo.IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
				P->SetStringField(TEXT("direction"), bEntry ? TEXT("input") : TEXT("output"));
				P->SetStringField(TEXT("category"), PinInfo->PinType.PinCategory.ToString());
				if (const UObject* SubObj = PinInfo->PinType.PinSubCategoryObject.Get())
				{
					P->SetStringField(TEXT("sub_type"), SubObj->GetName());
				}
				P->SetBoolField(TEXT("is_reference"), PinInfo->PinType.bIsReference);
				P->SetBoolField(TEXT("is_const"), PinInfo->PinType.bIsConst);
				ParamArr.Add(MakeShared<FJsonValueObject>(P));
			}
		}
		Root->SetArrayField(TEXT("params"), ParamArr);
	}
	return FMonolithActionResult::Success(Root);
}

// --- override_parent_function (gap #6) ---
// Authors a Blueprint override of a parent BlueprintImplementableEvent / BlueprintNativeEvent
// that RETURNS a value (e.g. UCommonActivatableWidget::BP_GetDesiredFocusTarget -> UWidget*).
// add_function can't do this (it makes a fresh graph, ignoring the parent signature) and the
// event-node form has no ReturnValue pin by design. Engine-generic: the declaring class is
// resolved by name via GetOverrideFunctionClass — nothing hardcoded.

FMonolithActionResult FMonolithBlueprintGraphActions::HandleOverrideParentFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ParentFuncName = Params->GetStringField(TEXT("parent_function_name"));
	if (ParentFuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: parent_function_name"));
	}

	// Resolve the parent class that DECLARES this overridable function.
	// Mirrors engine BlueprintEditor.cpp:6857-6870 / DataprepEditorUtils.cpp:211.
	UFunction* OverrideFunc = nullptr;
	UClass* const OverrideFuncClass =
		FBlueprintEditorUtils::GetOverrideFunctionClass(BP, FName(*ParentFuncName), &OverrideFunc);

	if (!OverrideFuncClass || !OverrideFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("'%s' is not an overridable function on the parent of %s (not found, or not a BlueprintImplementableEvent / BlueprintNativeEvent)."),
			*ParentFuncName, *BP->GetName()));
	}

	// Reject if an override graph with this name already exists.
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing->GetName() == ParentFuncName)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Override function graph already exists: %s"), *ParentFuncName));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*ParentFuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create override graph: %s"), *ParentFuncName));
	}

	// Override authoring uses the <UClass> template + the declaring class with
	// bIsUserCreated=false. (The <UFunction> overload is copy-signature, NOT override —
	// the gaps-doc had this backwards.) The entry/result nodes inherit the parent signature,
	// so a value-returning override gets its ReturnValue pin.
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, NewGraph, /*bIsUserCreated=*/false, OverrideFuncClass);

	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (!EntryNode)  EntryNode  = Cast<UK2Node_FunctionEntry>(Node);
		if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
	}

	// Locate the ReturnValue pin on the result node (the whole point of #6 vs the event form).
	FString ReturnPinId;
	FString ReturnPinName;
	if (ResultNode)
	{
		UEdGraphPin* ReturnPin = nullptr;
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
			{
				ReturnPin = Pin;
				break;
			}
		}
		// Fall back to the first non-exec input if the return isn't literally named ReturnValue.
		if (!ReturnPin)
		{
			for (UEdGraphPin* Pin : ResultNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					ReturnPin = Pin;
					break;
				}
			}
		}
		if (ReturnPin)
		{
			ReturnPinId = ReturnPin->PinId.ToString();
			ReturnPinName = ReturnPin->PinName.ToString();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetStringField(TEXT("entry_node_id"), EntryNode ? EntryNode->GetName() : FString());
	Root->SetStringField(TEXT("return_pin_id"), ReturnPinId);
	Root->SetStringField(TEXT("return_pin_name"), ReturnPinName);
	Root->SetStringField(TEXT("override_class"), OverrideFuncClass->GetName());
	Root->SetBoolField(TEXT("has_return_value"), !ReturnPinId.IsEmpty());
	return FMonolithActionResult::Success(Root);
}

// --- remove_function ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName = Params->GetStringField(TEXT("name"));
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Only search function graphs — not the event graph or macros
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_function"), FuncName);
	return FMonolithActionResult::Success(Root);
}

// --- set_function_thread_safe ---
// Sets/clears FKismetUserDeclaredFunctionMetadata::bThreadSafe on a function graph's entry node.
// The BP compiler turns bThreadSafe into the BlueprintThreadSafe function metadata, i.e. it ticks
// the "Thread Safe" checkbox programmatically. Mirrors HandleRemoveFunction's graph search and
// HandleAddFunction's entry-node-find pattern. Function graphs only (AnimBP function graphs also
// live in BP->FunctionGraphs).

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetFunctionThreadSafe(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	if (!Params->TryGetStringField(TEXT("function_name"), FuncName) || FuncName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("name"), FuncName);
	}
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	bool bThreadSafe = true;
	Params->TryGetBoolField(TEXT("thread_safe"), bThreadSafe);

	// Function graphs only (covers AnimBP function graphs, which also live here).
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	// Find the entry node (mirror HandleAddFunction).
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No function entry node found in graph: %s"), *FuncName));
	}

	EntryNode->Modify();
	EntryNode->MetaData.bThreadSafe = bThreadSafe;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetBoolField(TEXT("thread_safe"), EntryNode->MetaData.bThreadSafe);
	return FMonolithActionResult::Success(Root);
}

// --- function entry / flag helpers (rename_function, set_function_flags) ---

namespace
{
	UK2Node_FunctionEntry* MonolithGraphActionsFindFunctionEntry(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
			{
				return EntryNode;
			}
		}
		return nullptr;
	}

	void MonolithGraphActionsWriteFunctionFlags(const UK2Node_FunctionEntry* EntryNode, const TSharedPtr<FJsonObject>& Out)
	{
		const uint32 Flags = (uint32)EntryNode->GetFunctionFlags();
		Out->SetBoolField(TEXT("is_pure"), (Flags & FUNC_BlueprintPure) != 0);
		Out->SetBoolField(TEXT("is_const"), (Flags & FUNC_Const) != 0);
		Out->SetBoolField(TEXT("is_static"), (Flags & FUNC_Static) != 0);
		Out->SetBoolField(TEXT("call_in_editor"), EntryNode->MetaData.bCallInEditor);
		Out->SetBoolField(TEXT("thread_safe"), EntryNode->MetaData.bThreadSafe);

		const TCHAR* AccessStr = TEXT("Public");
		if (Flags & FUNC_Private)        AccessStr = TEXT("Private");
		else if (Flags & FUNC_Protected) AccessStr = TEXT("Protected");
		Out->SetStringField(TEXT("access"), AccessStr);
	}
}

// --- rename_function ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRenameFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString OldName = Params->GetStringField(TEXT("old_name"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	if (OldName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name"));
	}
	if (NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"));
	}

	UEdGraph* Graph = MonolithBlueprintInternal::FindGraphByName(BP, OldName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *OldName));
	}

	// Ensure we're only renaming function graphs, not event graphs or macros
	if (!BP->FunctionGraphs.Contains(Graph))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Graph '%s' is not a function graph (cannot rename event graphs or macros this way)"), *OldName));
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->FunctionGraphs)
	{
		if (Existing && Existing != Graph && Existing->GetName() == NewName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("A function named '%s' already exists"), *NewName));
		}
	}

	// Snapshot the entry node's user-facing flags and metadata so the rename
	// cannot lose pure/const/access/CallInEditor/category/tooltip/thread-safe.
	UK2Node_FunctionEntry* EntryNode = MonolithGraphActionsFindFunctionEntry(Graph);
	const int32 SavedExtraFlags = EntryNode ? EntryNode->GetExtraFlags() : 0;
	const FKismetUserDeclaredFunctionMetadata SavedMetaData =
		EntryNode ? EntryNode->MetaData : FKismetUserDeclaredFunctionMetadata();

	FBlueprintEditorUtils::RenameGraph(Graph, NewName);

	// RenameGraph returns silently (leaving the old name) when the rename test fails.
	if (Graph->GetName() != NewName)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Rename of '%s' to '%s' was rejected (name collides with an existing object in the Blueprint)"),
			*OldName, *NewName));
	}

	if (EntryNode)
	{
		EntryNode->Modify();
		EntryNode->SetExtraFlags(SavedExtraFlags);
		EntryNode->MetaData = SavedMetaData;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), Graph->GetName());
	if (EntryNode)
	{
		MonolithGraphActionsWriteFunctionFlags(EntryNode, Root);
	}
	return FMonolithActionResult::Success(Root);
}

// --- set_function_flags ---
// Edits pure / const / access / CallInEditor / category / tooltip on an existing function graph.
// Mirrors FBlueprintGraphActionDetails (BlueprintDetailsCustomization.cpp): the entry node's
// ExtraFlags and MetaData are the persistent source of truth, and the skeleton UFunction is
// updated alongside so call sites see the new purity before the next compile.

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetFunctionFlags(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName;
	if (!Params->TryGetStringField(TEXT("function_name"), FuncName) || FuncName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("name"), FuncName);
	}
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	UK2Node_FunctionEntry* EntryNode = MonolithGraphActionsFindFunctionEntry(Graph);
	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No function entry node found in graph: %s"), *FuncName));
	}

	bool bSetPure = false, bPure = false;
	bool bSetConst = false, bConst = false;
	bool bSetCallInEditor = false, bCallInEditor = false;
	bSetPure         = Params->TryGetBoolField(TEXT("is_pure"), bPure);
	bSetConst        = Params->TryGetBoolField(TEXT("is_const"), bConst);
	bSetCallInEditor = Params->TryGetBoolField(TEXT("call_in_editor"), bCallInEditor);

	FString Access;
	const bool bSetAccess = Params->TryGetStringField(TEXT("access"), Access) && !Access.IsEmpty();
	FString Category;
	const bool bSetCategory = Params->TryGetStringField(TEXT("category"), Category);
	FString Description;
	const bool bSetDescription = Params->TryGetStringField(TEXT("description"), Description);

	if (!bSetPure && !bSetConst && !bSetCallInEditor && !bSetAccess && !bSetCategory && !bSetDescription)
	{
		return FMonolithActionResult::Error(TEXT(
			"Nothing to change: pass at least one of is_pure, is_const, call_in_editor, access, category, description"));
	}

	if ((bSetPure || bSetConst || bSetAccess) && !EntryNode->IsEditable())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Function '%s' has a non-editable signature (override or interface function); pure/const/access cannot be changed"),
			*FuncName));
	}

	uint32 AccessFlag = 0;
	if (bSetAccess)
	{
		if (Access.Equals(TEXT("Public"), ESearchCase::IgnoreCase))          AccessFlag = FUNC_Public;
		else if (Access.Equals(TEXT("Protected"), ESearchCase::IgnoreCase))  AccessFlag = FUNC_Protected;
		else if (Access.Equals(TEXT("Private"), ESearchCase::IgnoreCase))    AccessFlag = FUNC_Private;
		else
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid access '%s' (expected Public, Protected, or Private)"), *Access));
		}
	}

	if (bSetConst && bConst && (EntryNode->GetFunctionFlags() & FUNC_Static))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Function '%s' is static; static functions cannot be const"), *FuncName));
	}

	UFunction* SkeletonFunction = BP->SkeletonGeneratedClass
		? BP->SkeletonGeneratedClass->FindFunctionByName(Graph->GetFName(), EIncludeSuperFlag::ExcludeSuper)
		: nullptr;

	const bool bWasPure = (EntryNode->GetFunctionFlags() & FUNC_BlueprintPure) != 0;

	EntryNode->Modify();
	if (SkeletonFunction)
	{
		SkeletonFunction->Modify();
	}

	uint32 ExtraFlags = (uint32)EntryNode->GetExtraFlags();
	uint32 SetMask = 0;
	uint32 ClearMask = 0;

	if (bSetPure)
	{
		if (bPure) SetMask |= FUNC_BlueprintPure; else ClearMask |= FUNC_BlueprintPure;
	}
	if (bSetConst)
	{
		if (bConst) SetMask |= FUNC_Const; else ClearMask |= FUNC_Const;
	}
	if (bSetAccess)
	{
		ClearMask |= FUNC_AccessSpecifiers;
		SetMask |= AccessFlag;
	}

	ExtraFlags = (ExtraFlags & ~ClearMask) | SetMask;
	EntryNode->SetExtraFlags((int32)ExtraFlags);
	if (SkeletonFunction)
	{
		SkeletonFunction->FunctionFlags &= (EFunctionFlags)~ClearMask;
		SkeletonFunction->FunctionFlags |= (EFunctionFlags)SetMask;
	}

	if (bSetCallInEditor)
	{
		EntryNode->MetaData.bCallInEditor = bCallInEditor;
	}
	if (bSetCategory)
	{
		EntryNode->MetaData.Category = FText::FromString(Category);
	}
	if (bSetDescription)
	{
		EntryNode->MetaData.ToolTip = FText::FromString(Description);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// A purity change adds or removes exec pins on this Blueprint's own call sites.
	const bool bIsPureNow = (EntryNode->GetFunctionFlags() & FUNC_BlueprintPure) != 0;
	int32 RefreshedCallSites = 0;
	if (bIsPureNow != bWasPure)
	{
		TArray<UK2Node_CallFunction*> CallNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CallFunction>(BP, CallNodes);
		for (UK2Node_CallFunction* CallNode : CallNodes)
		{
			if (CallNode && CallNode->FunctionReference.IsSelfContext() &&
				CallNode->FunctionReference.GetMemberName() == Graph->GetFName())
			{
				CallNode->ReconstructNode();
				++RefreshedCallSites;
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	MonolithGraphActionsWriteFunctionFlags(EntryNode, Root);
	Root->SetStringField(TEXT("category"), EntryNode->MetaData.Category.ToString());
	Root->SetStringField(TEXT("description"), EntryNode->MetaData.ToolTip.ToString());
	if (RefreshedCallSites > 0)
	{
		Root->SetNumberField(TEXT("refreshed_call_sites"), RefreshedCallSites);
	}
	return FMonolithActionResult::Success(Root);
}

// --- add_event_graph ---
// Mirrors FBlueprintEditor::NewDocument_OnClicked(CGT_NewEventGraph): CreateNewGraph +
// AddUbergraphPage (which marks the Blueprint structurally modified).

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddEventGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!FBlueprintEditorUtils::DoesSupportEventGraphs(BP))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint '%s' does not support event graphs (e.g. function libraries, interfaces, macro libraries)"),
			*BP->GetName()));
	}

	FString RequestedName;
	if (!Params->TryGetStringField(TEXT("name"), RequestedName) || RequestedName.IsEmpty())
	{
		Params->TryGetStringField(TEXT("graph_name"), RequestedName);
	}

	FName GraphName;
	if (RequestedName.IsEmpty())
	{
		GraphName = FBlueprintEditorUtils::FindUniqueKismetName(BP, TEXT("NewEventGraph"));
	}
	else
	{
		FKismetNameValidator Validator(BP);
		const EValidatorResult Result = Validator.IsValid(RequestedName);
		if (Result != EValidatorResult::Ok)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid event graph name '%s': %s"),
				*RequestedName, *INameValidatorInterface::GetErrorString(RequestedName, Result)));
		}
		GraphName = FName(*RequestedName);
	}

	// Reuse the schema of the existing event graph (AnimBPs use a K2 subclass).
	TSubclassOf<UEdGraphSchema> SchemaClass = UEdGraphSchema_K2::StaticClass();
	for (const UEdGraph* Existing : BP->UbergraphPages)
	{
		if (Existing && Existing->Schema)
		{
			SchemaClass = Existing->Schema;
			break;
		}
	}

	BP->Modify();
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, GraphName, UEdGraph::StaticClass(), SchemaClass);
	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create event graph: %s"), *GraphName.ToString()));
	}

	FBlueprintEditorUtils::AddUbergraphPage(BP, NewGraph);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetStringField(TEXT("graph_type"), TEXT("event_graph"));
	Root->SetNumberField(TEXT("event_graph_count"), BP->UbergraphPages.Num());
	return FMonolithActionResult::Success(Root);
}

// --- add_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString MacroName = Params->GetStringField(TEXT("name"));
	if (MacroName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Check for name collision
	for (const UEdGraph* Existing : BP->MacroGraphs)
	{
		if (Existing && Existing->GetName() == MacroName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Macro already exists: %s"), *MacroName));
		}
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create macro graph: %s"), *MacroName));
	}

	FBlueprintEditorUtils::AddMacroGraph(BP, NewGraph, /*bIsUserCreated=*/true, nullptr);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	Root->SetNumberField(TEXT("node_count"), NewGraph->Nodes.Num());
	return FMonolithActionResult::Success(Root);
}

// --- remove_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString MacroName = Params->GetStringField(TEXT("macro_name"));
	if (MacroName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: macro_name"));
	}

	// Only search macro graphs — not function graphs or event graphs
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->MacroGraphs)
	{
		if (G && G->GetName() == MacroName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Macro not found: %s"), *MacroName));
	}

	FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::Recompile);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_macro"), MacroName);
	return FMonolithActionResult::Success(Root);
}

// --- rename_macro ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRenameMacro(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString OldName = Params->GetStringField(TEXT("old_name"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	if (OldName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: old_name"));
	}
	if (NewName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_name"));
	}

	// Search macro graphs specifically
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->MacroGraphs)
	{
		if (G && G->GetName() == OldName)
		{
			Graph = G;
			break;
		}
	}

	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Macro not found: %s"), *OldName));
	}

	// Check for name collision within macro graphs
	for (const UEdGraph* Existing : BP->MacroGraphs)
	{
		if (Existing && Existing != Graph && Existing->GetName() == NewName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("A macro named '%s' already exists"), *NewName));
		}
	}

	FBlueprintEditorUtils::RenameGraph(Graph, *NewName);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), OldName);
	Root->SetStringField(TEXT("new_name"), Graph->GetName());
	return FMonolithActionResult::Success(Root);
}

// --- add_event_dispatcher ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleAddEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName = Params->GetStringField(TEXT("name"));
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: name"));
	}

	// Generate a unique name for the delegate signature graph
	FName UniqueName = FBlueprintEditorUtils::FindUniqueKismetName(BP, DispatcherName);

	// Check for name collision against the display name (without _Signature suffix)
	for (const UEdGraph* Existing : BP->DelegateSignatureGraphs)
	{
		if (!Existing) continue;
		FString ExistingDisplay = Existing->GetName();
		if (ExistingDisplay.EndsWith(TEXT("_Signature")))
		{
			ExistingDisplay.LeftChopInline(10, EAllowShrinking::No);
		}
		if (ExistingDisplay == DispatcherName)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Event dispatcher already exists: %s"), *DispatcherName));
		}
	}

	// The multicast delegate member variable is what the Blueprint compiler turns into the
	// FMulticastDelegateProperty on the generated class — without it the dispatcher exists only
	// as a signature graph and CallDelegate/AddDelegate can never bind it. Mirrors
	// FBlueprintEditor::OnAddNewDelegate (variable first, graph second, rollback on failure).
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(BP, UniqueName, DelegateType))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add delegate member variable: %s"), *DispatcherName));
	}

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, UniqueName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

	if (!NewGraph)
	{
		FBlueprintEditorUtils::RemoveMemberVariable(BP, UniqueName);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create delegate signature graph: %s"), *DispatcherName));
	}

	NewGraph->bEditable = false;

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*NewGraph);
	K2Schema->CreateFunctionGraphTerminators(*NewGraph, (UClass*)nullptr);
	K2Schema->AddExtraFunctionFlags(NewGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
	K2Schema->MarkFunctionEntryAsEditable(NewGraph, true);

	BP->DelegateSignatureGraphs.Add(NewGraph);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Compute display name (strip _Signature if UE added it)
	FString DisplayName = NewGraph->GetName();
	if (DisplayName.EndsWith(TEXT("_Signature")))
	{
		DisplayName.LeftChopInline(10, EAllowShrinking::No);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("dispatcher_name"), DisplayName);
	Root->SetStringField(TEXT("graph_name"), NewGraph->GetName());
	return FMonolithActionResult::Success(Root);
}

// --- function signature helpers (set_function_params, remove_function_param, reorder_function_params) ---

namespace
{
	UEdGraph* MonolithGraphActionsFindFunctionGraph(UBlueprint* BP, const FString& FuncName)
	{
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G && G->GetName() == FuncName)
			{
				return G;
			}
		}
		return nullptr;
	}

	FString MonolithGraphActionsListFunctionGraphs(UBlueprint* BP)
	{
		TArray<FString> Names;
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G) Names.Add(G->GetName());
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	FString MonolithGraphActionsListUserPins(const UK2Node_EditablePinBase* Node)
	{
		TArray<FString> Names;
		if (Node)
		{
			for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
			{
				if (Info.IsValid()) Names.Add(Info->PinName.ToString());
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : TEXT("(none)");
	}

	// Push a user-defined pin edit through the same path as the My Blueprint
	// details panel (FBaseBlueprintGraphActionDetails::OnParamsChanged): rebuild
	// the entry/result node without keeping orphans, then let the schema mark the
	// Blueprint structurally modified and reconstruct every call site, macro
	// instance and delegate node that uses the signature (FParamsChangedHelper).
	void MonolithGraphActionsApplySignatureChange(UK2Node_EditablePinBase* Node)
	{
		if (!Node)
		{
			return;
		}
		const bool bPrevDisableOrphanSaving = Node->bDisableOrphanPinSaving;
		Node->bDisableOrphanPinSaving = true;
		Node->ReconstructNode();
		Node->bDisableOrphanPinSaving = bPrevDisableOrphanSaving;

		GetDefault<UEdGraphSchema_K2>()->HandleParameterDefaultValueChanged(Node);
	}

	// After a parameter is removed, the call-site reconstruct keeps the old pin as
	// an orphan. A linked orphan is a compile error ("In use pin ... no longer
	// exists"), so call sites of this function that carry an orphan for the removed
	// parameter are rebuilt once more with orphan saving disabled. Covers every
	// loaded Blueprint, matching FParamsChangedHelper's scope. Returns the number
	// of call nodes cleaned.
	int32 MonolithGraphActionsDropStaleCallSitePins(UBlueprint* BP, FName FuncName, FName ParamName,
		TArray<TSharedPtr<FJsonValue>>& OutCallSites)
	{
		TArray<UK2Node_CallFunction*> ToRebuild;
		for (TObjectIterator<UK2Node_CallFunction> It(RF_Transient | RF_ClassDefaultObject); It; ++It)
		{
			UK2Node_CallFunction* Call = *It;
			if (!IsValid(Call) || !Call->GetOuter() || !Call->GetOuter()->IsA<UEdGraph>() || Call->FunctionReference.GetMemberName() != FuncName)
			{
				continue;
			}

			UBlueprint* OwnerBP = nullptr;
			if (const UFunction* Target = Call->GetTargetFunction())
			{
				OwnerBP = UBlueprint::GetBlueprintFromClass(Target->GetOwnerClass());
			}
			else
			{
				OwnerBP = FBlueprintEditorUtils::FindBlueprintForNode(Call);
			}
			if (OwnerBP != BP)
			{
				continue;
			}

			for (const UEdGraphPin* Pin : Call->Pins)
			{
				if (!Pin || !Pin->bOrphanedPin) continue;
				const UEdGraphPin* RootPin = Pin;
				while (RootPin->ParentPin) RootPin = RootPin->ParentPin;
				if (RootPin->PinName == ParamName)
				{
					ToRebuild.Add(Call);
					break;
				}
			}
		}

		for (UK2Node_CallFunction* Call : ToRebuild)
		{
			Call->Modify();
			const bool bPrev = Call->bDisableOrphanPinSaving;
			Call->bDisableOrphanPinSaving = true;
			Call->ReconstructNode();
			Call->bDisableOrphanPinSaving = bPrev;

			UBlueprint* CallerBP = FBlueprintEditorUtils::FindBlueprintForNode(Call);
			if (CallerBP)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(CallerBP);
			}

			TSharedPtr<FJsonObject> Site = MakeShared<FJsonObject>();
			Site->SetStringField(TEXT("node_id"), Call->GetName());
			Site->SetStringField(TEXT("graph"), Call->GetGraph()->GetName());
			Site->SetStringField(TEXT("blueprint"), CallerBP ? CallerBP->GetPathName() : FString());
			OutCallSites.Add(MakeShared<FJsonValueObject>(Site));
		}
		return ToRebuild.Num();
	}
}

// --- set_function_params ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetFunctionParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString FuncName = Params->GetStringField(TEXT("function_name"));
	if (FuncName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	}

	// Only function graphs can have params set this way
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName)
		{
			Graph = G;
			break;
		}
	}
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s"), *FuncName));
	}

	// Find entry and result nodes
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (!ResultNode) ResultNode = Cast<UK2Node_FunctionResult>(Node);
		if (EntryNode && ResultNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No FunctionEntry node found in: %s"), *FuncName));
	}

	// Parse EVERY requested pin type BEFORE touching the graph. Strict parse — a
	// typo'd type used to fall back to a silent bool pin — and a type error found
	// mid-loop would otherwise leave the function with a partial pin set that a
	// retry then duplicates.
	auto ParsePinArray = [&Params](const TCHAR* FieldName, const TCHAR* Label, bool bAllowRefFlags,
		TArray<TPair<FName, FEdGraphPinType>>& OutParsed, FString& OutError) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* PinArray = nullptr;
		if (!Params->TryGetArrayField(FieldName, PinArray) || !PinArray)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& PinVal : *PinArray)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			if (!PinVal->TryGetObject(PinObj) || !PinObj) continue;

			FString PinName, TypeStr;
			(*PinObj)->TryGetStringField(TEXT("name"), PinName);
			(*PinObj)->TryGetStringField(TEXT("type"), TypeStr);

			if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;

			FEdGraphPinType PinType;
			FString TypeError;
			if (!MonolithPinTypeGrammar::TryParsePinType(TypeStr, PinType, TypeError))
			{
				OutError = FString::Printf(TEXT("%s '%s' has an invalid type '%s': %s"),
					Label, *PinName, *TypeStr, *TypeError);
				return false;
			}
			// Optional pass-by-reference / const flags (FEdGraphPinType::bIsReference /
			// bIsConst). Only meaningful on inputs (entry-node pins): a `const FFoo&`
			// parameter is bIsReference + bIsConst. Outputs are already passed by
			// reference by the compiler, so the flags are rejected there.
			bool bIsReference = false;
			bool bIsConstPin = false;
			const bool bHasRef = (*PinObj)->TryGetBoolField(TEXT("is_reference"), bIsReference);
			const bool bHasConst = (*PinObj)->TryGetBoolField(TEXT("is_const"), bIsConstPin);
			if (!bAllowRefFlags && ((bHasRef && bIsReference) || (bHasConst && bIsConstPin)))
			{
				OutError = FString::Printf(TEXT("%s '%s': is_reference / is_const are only supported on inputs"),
					Label, *PinName);
				return false;
			}
			if (bIsConstPin && !bIsReference && !PinType.bIsReference)
			{
				OutError = FString::Printf(TEXT("%s '%s': is_const requires is_reference=true (Blueprint const parameters are const references)"),
					Label, *PinName);
				return false;
			}
			if (bHasRef)   PinType.bIsReference = bIsReference;
			if (bHasConst) PinType.bIsConst = bIsConstPin;

			OutParsed.Emplace(FName(*PinName), MoveTemp(PinType));
		}
		return true;
	};

	TArray<TPair<FName, FEdGraphPinType>> NewInputs;
	TArray<TPair<FName, FEdGraphPinType>> NewOutputs;
	FString ParseError;
	if (!ParsePinArray(TEXT("inputs"), TEXT("Input"), /*bAllowRefFlags=*/true, NewInputs, ParseError) ||
		!ParsePinArray(TEXT("outputs"), TEXT("Output"), /*bAllowRefFlags=*/false, NewOutputs, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	int32 InputsAdded = 0;
	int32 OutputsAdded = 0;

	// Process inputs — add as user-defined pins on the entry node
	for (const TPair<FName, FEdGraphPinType>& Input : NewInputs)
	{
		EntryNode->CreateUserDefinedPin(Input.Key, Input.Value, EGPD_Output);
		++InputsAdded;
	}

	// Process outputs — add as user-defined pins on the result node.
	// Keyed on the FIELD being present, not on NewOutputs being non-empty, so an
	// empty "outputs" array still materialises the result node as it always did.
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray)
	{
		if (!ResultNode)
		{
			// Create a result node if one doesn't exist
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
			ResultNode = Creator.CreateNode();
			ResultNode->NodePosX = EntryNode ? EntryNode->NodePosX + 400 : 0;
			ResultNode->NodePosY = EntryNode ? EntryNode->NodePosY : 0;
			Creator.Finalize();
		}

		for (const TPair<FName, FEdGraphPinType>& Output : NewOutputs)
		{
			ResultNode->CreateUserDefinedPin(Output.Key, Output.Value, EGPD_Input);
			++OutputsAdded;
		}
	}

	if (InputsAdded == 0 && OutputsAdded == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid inputs or outputs provided"));
	}

	// Rebuild the signature nodes and refresh every call site (issue #162), the
	// same path the details panel takes when a parameter is added.
	if (InputsAdded > 0)
	{
		MonolithGraphActionsApplySignatureChange(EntryNode);
	}
	if (OutputsAdded > 0 && ResultNode)
	{
		MonolithGraphActionsApplySignatureChange(ResultNode);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetNumberField(TEXT("inputs_added"), InputsAdded);
	Root->SetNumberField(TEXT("outputs_added"), OutputsAdded);
	return FMonolithActionResult::Success(Root);
}

// --- remove_function_param ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveFunctionParam(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString FuncName = Params->GetStringField(TEXT("function_name"));
	const FString ParamNameStr = Params->GetStringField(TEXT("param_name"));
	if (FuncName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));
	if (ParamNameStr.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: param_name"));

	FString Direction = TEXT("any");
	Params->TryGetStringField(TEXT("direction"), Direction);
	Direction = Direction.ToLower();
	if (Direction != TEXT("any") && Direction != TEXT("input") && Direction != TEXT("output"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid direction '%s'. Use 'input', 'output' or 'any'."), *Direction));
	}

	UEdGraph* Graph = MonolithGraphActionsFindFunctionGraph(BP, FuncName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s. Function graphs: %s"),
			*FuncName, *MonolithGraphActionsListFunctionGraphs(BP)));
	}

	UK2Node_FunctionEntry* EntryNode = MonolithGraphActionsFindFunctionEntry(Graph);
	UK2Node_FunctionResult* FirstResult = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		FirstResult = Cast<UK2Node_FunctionResult>(Node);
		if (FirstResult) break;
	}
	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No FunctionEntry node found in: %s"), *FuncName));
	}

	// Case-insensitive match against the declared parameters, resolving the
	// stored spelling so FName comparisons downstream use the real name.
	auto FindUserPin = [&ParamNameStr](const UK2Node_EditablePinBase* Node) -> TSharedPtr<FUserPinInfo>
	{
		if (!Node) return nullptr;
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName.ToString().Equals(ParamNameStr, ESearchCase::IgnoreCase))
			{
				return Info;
			}
		}
		return nullptr;
	};

	TSharedPtr<FUserPinInfo> InputInfo;
	TSharedPtr<FUserPinInfo> OutputInfo;
	if (Direction != TEXT("output"))
	{
		InputInfo = FindUserPin(EntryNode);
	}
	if (Direction != TEXT("input") && !InputInfo.IsValid())
	{
		OutputInfo = FindUserPin(FirstResult);
	}
	if (!InputInfo.IsValid() && !OutputInfo.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Parameter '%s' not found on function '%s' (direction: %s). Inputs: %s. Outputs: %s."),
			*ParamNameStr, *FuncName, *Direction,
			*MonolithGraphActionsListUserPins(EntryNode), *MonolithGraphActionsListUserPins(FirstResult)));
	}

	const bool bIsInput = InputInfo.IsValid();
	const FName ParamName = bIsInput ? InputInfo->PinName : OutputInfo->PinName;

	// Get/Set nodes bound to the input inside the function (local member scope,
	// no guid -- see add_node VariableGet, issue #159) become invalid once the
	// parameter is gone. Like the editor, they are left in place and reported.
	TArray<TSharedPtr<FJsonValue>> DanglingVarNodes;
	TArray<TSharedPtr<FJsonValue>> LinkedDanglingVarNodes;
	if (bIsInput)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node);
			if (VarNode && VarNode->VariableReference.IsLocalScope() &&
				VarNode->VariableReference.GetMemberName() == ParamName &&
				!VarNode->VariableReference.GetMemberGuid().IsValid())
			{
				DanglingVarNodes.Add(MakeShared<FJsonValueString>(VarNode->GetName()));
				// Unconnected Get/Set nodes are pruned before compile and are harmless;
				// only connected ones reference the missing parameter in compiled code.
				const bool bWired = VarNode->Pins.ContainsByPredicate([](const UEdGraphPin* Pin)
				{
					return Pin && Pin->LinkedTo.Num() > 0;
				});
				if (bWired)
				{
					LinkedDanglingVarNodes.Add(MakeShared<FJsonValueString>(VarNode->GetName()));
				}
			}
		}
	}

	if (bIsInput)
	{
		EntryNode->Modify();
		EntryNode->RemoveUserDefinedPinByName(ParamName);
		MonolithGraphActionsApplySignatureChange(EntryNode);
	}
	else
	{
		// Every return node of the function carries the same outputs; the details
		// panel edits all of them together (GatherAllResultNodes).
		for (UK2Node_FunctionResult* ResultNode : FirstResult->GetAllResultNodes())
		{
			if (!ResultNode) continue;
			ResultNode->Modify();
			ResultNode->RemoveUserDefinedPinByName(ParamName);
			MonolithGraphActionsApplySignatureChange(ResultNode);
		}
	}

	TArray<TSharedPtr<FJsonValue>> CleanedCallSites;
	MonolithGraphActionsDropStaleCallSitePins(BP, FName(*FuncName), ParamName, CleanedCallSites);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetStringField(TEXT("removed_param"), ParamName.ToString());
	Root->SetStringField(TEXT("direction"), bIsInput ? TEXT("input") : TEXT("output"));
	Root->SetStringField(TEXT("inputs"), MonolithGraphActionsListUserPins(EntryNode));
	Root->SetStringField(TEXT("outputs"), MonolithGraphActionsListUserPins(FirstResult));
	Root->SetArrayField(TEXT("call_sites_cleaned"), CleanedCallSites);
	if (DanglingVarNodes.Num() > 0)
	{
		Root->SetArrayField(TEXT("dangling_parameter_nodes"), DanglingVarNodes);
		Root->SetArrayField(TEXT("linked_dangling_parameter_nodes"), LinkedDanglingVarNodes);
		if (LinkedDanglingVarNodes.Num() > 0)
		{
			Root->SetStringField(TEXT("warning"), FString::Printf(
				TEXT("%d of %d Get/Set node(s) for the removed parameter are still connected (listed in linked_dangling_parameter_nodes); the function will not compile until they are removed with remove_node or rewired. Unconnected ones are ignored by the compiler."),
				LinkedDanglingVarNodes.Num(), DanglingVarNodes.Num()));
		}
		else
		{
			Root->SetStringField(TEXT("warning"),
				TEXT("Unconnected Get/Set nodes for the removed parameter remain in the function graph. They do not affect compilation; remove them with remove_node to tidy the graph."));
		}
	}
	return FMonolithActionResult::Success(Root);
}

// --- reorder_function_params ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleReorderFunctionParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const FString FuncName = Params->GetStringField(TEXT("function_name"));
	if (FuncName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: function_name"));

	FString Direction = TEXT("input");
	Params->TryGetStringField(TEXT("direction"), Direction);
	Direction = Direction.ToLower();
	if (Direction == TEXT("inputs")) Direction = TEXT("input");
	if (Direction == TEXT("outputs")) Direction = TEXT("output");
	if (Direction != TEXT("input") && Direction != TEXT("output"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid direction '%s'. Use 'input' or 'output'."), *Direction));
	}

	const TArray<TSharedPtr<FJsonValue>>* OrderArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("order"), OrderArray) || !OrderArray || OrderArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: order (array of parameter names)"));
	}

	UEdGraph* Graph = MonolithGraphActionsFindFunctionGraph(BP, FuncName);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function not found: %s. Function graphs: %s"),
			*FuncName, *MonolithGraphActionsListFunctionGraphs(BP)));
	}

	TArray<UK2Node_EditablePinBase*> TargetNodes;
	if (Direction == TEXT("input"))
	{
		if (UK2Node_FunctionEntry* EntryNode = MonolithGraphActionsFindFunctionEntry(Graph))
		{
			TargetNodes.Add(EntryNode);
		}
	}
	else
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
			{
				for (UK2Node_FunctionResult* R : ResultNode->GetAllResultNodes())
				{
					if (R) TargetNodes.AddUnique(R);
				}
				break;
			}
		}
	}
	if (TargetNodes.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Function '%s' has no %s node"),
			*FuncName, Direction == TEXT("input") ? TEXT("entry") : TEXT("result")));
	}

	// Names listed move to the front in the given order; unlisted parameters
	// keep their relative order after them.
	const UK2Node_EditablePinBase* Reference = TargetNodes[0];
	TArray<FName> RequestedOrder;
	for (const TSharedPtr<FJsonValue>& Val : *OrderArray)
	{
		FString Name;
		if (!Val.IsValid() || !Val->TryGetString(Name) || Name.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("order must be an array of non-empty parameter name strings"));
		}
		FName Resolved = NAME_None;
		for (const TSharedPtr<FUserPinInfo>& Info : Reference->UserDefinedPins)
		{
			if (Info.IsValid() && Info->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				Resolved = Info->PinName;
				break;
			}
		}
		if (Resolved.IsNone())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter '%s' is not an %s of '%s'. Current %ss: %s"),
				*Name, *Direction, *FuncName, *Direction, *MonolithGraphActionsListUserPins(Reference)));
		}
		if (RequestedOrder.Contains(Resolved))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter '%s' is listed more than once in order"), *Name));
		}
		RequestedOrder.Add(Resolved);
	}

	bool bChanged = false;
	for (UK2Node_EditablePinBase* Node : TargetNodes)
	{
		TArray<TSharedPtr<FUserPinInfo>> Reordered;
		for (const FName& Name : RequestedOrder)
		{
			for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
			{
				if (Info.IsValid() && Info->PinName == Name)
				{
					Reordered.Add(Info);
					break;
				}
			}
		}
		for (const TSharedPtr<FUserPinInfo>& Info : Node->UserDefinedPins)
		{
			if (!Reordered.Contains(Info))
			{
				Reordered.Add(Info);
			}
		}
		if (Reordered == Node->UserDefinedPins)
		{
			continue;
		}
		Node->Modify();
		Node->UserDefinedPins = MoveTemp(Reordered);
		MonolithGraphActionsApplySignatureChange(Node);
		bChanged = true;
	}

	if (bChanged)
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("function_name"), FuncName);
	Root->SetStringField(TEXT("direction"), Direction);
	Root->SetBoolField(TEXT("changed"), bChanged);
	TArray<TSharedPtr<FJsonValue>> FinalOrder;
	for (const TSharedPtr<FUserPinInfo>& Info : TargetNodes[0]->UserDefinedPins)
	{
		if (Info.IsValid()) FinalOrder.Add(MakeShared<FJsonValueString>(Info->PinName.ToString()));
	}
	Root->SetArrayField(TEXT("order"), FinalOrder);
	return FMonolithActionResult::Success(Root);
}

// --- implement_interface ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleImplementInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName = Params->GetStringField(TEXT("interface_class"));
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Verify the class exists before attempting to add it
	UClass* InterfaceClass = FindFirstObject<UClass>(*InterfaceClassName, EFindFirstObjectOptions::NativeFirst);
	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Interface class not found: %s"), *InterfaceClassName));
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not an interface"), *InterfaceClassName));
	}

	// Check if already implemented
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface == InterfaceClass)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Interface already implemented: %s"), *InterfaceClassName));
		}
	}

	const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, InterfaceClass->GetClassPathName());
	if (!bAdded)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to implement interface: %s"), *InterfaceClassName));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_class"), InterfaceClassName);
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	return FMonolithActionResult::Success(Root);
}

// --- remove_interface ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName = Params->GetStringField(TEXT("interface_class"));
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Verify the interface is actually implemented
	UClass* InterfaceClass = nullptr;
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface && Existing.Interface->GetName() == InterfaceClassName)
		{
			InterfaceClass = Existing.Interface;
			break;
		}
	}

	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Interface not implemented by this Blueprint: %s"), *InterfaceClassName));
	}

	bool bPreserveFunctions = false;
	Params->TryGetBoolField(TEXT("preserve_functions"), bPreserveFunctions);

	FBlueprintEditorUtils::RemoveInterface(BP, InterfaceClass->GetClassPathName(), bPreserveFunctions);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_class"), InterfaceClassName);
	Root->SetBoolField(TEXT("functions_preserved"), bPreserveFunctions);
	return FMonolithActionResult::Success(Root);
}

// --- scaffold_interface_implementation ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleScaffoldInterfaceImplementation(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString InterfaceClassName = Params->GetStringField(TEXT("interface_class"));
	if (InterfaceClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: interface_class"));
	}

	// Resolve the interface class — try as-is, then strip leading 'I', then add 'U' prefix
	UClass* InterfaceClass = FindFirstObject<UClass>(*InterfaceClassName, EFindFirstObjectOptions::NativeFirst);
	if (!InterfaceClass && InterfaceClassName.StartsWith(TEXT("I")))
	{
		// Blueprint interfaces typically have U prefix in the class system (e.g. IBpi_Interactable -> UBpi_Interactable)
		InterfaceClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *InterfaceClassName.Mid(1)), EFindFirstObjectOptions::NativeFirst);
	}
	if (!InterfaceClass)
	{
		InterfaceClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *InterfaceClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!InterfaceClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Interface class not found: %s"), *InterfaceClassName));
	}

	if (!InterfaceClass->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Class '%s' is not an interface"), *InterfaceClassName));
	}

	// Check if already implemented
	bool bAlreadyImplemented = false;
	for (const FBPInterfaceDescription& Existing : BP->ImplementedInterfaces)
	{
		if (Existing.Interface == InterfaceClass)
		{
			bAlreadyImplemented = true;
			break;
		}
	}

	// Snapshot function graph names before implementing so we can detect which were newly created
	TSet<FName> GraphsBefore;
	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (G) GraphsBefore.Add(G->GetFName());
	}
	TSet<FName> UbergraphsBefore;
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (G) UbergraphsBefore.Add(G->GetFName());
	}

	if (!bAlreadyImplemented)
	{
		// ImplementNewInterface requires FTopLevelAssetPath (not the deprecated FName overload)
		const bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, InterfaceClass->GetClassPathName());
		if (!bAdded)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("ImplementNewInterface failed for: %s"), *InterfaceClassName));
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	// Collect newly created graphs — compare against pre-implementation snapshots
	// Functions with return values get function graphs; void functions become event nodes in the ubergraph
	TArray<TSharedPtr<FJsonValue>> FunctionsCreated;

	for (const UEdGraph* G : BP->FunctionGraphs)
	{
		if (!G) continue;
		if (GraphsBefore.Contains(G->GetFName())) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), G->GetName());
		Entry->SetStringField(TEXT("graph_name"), G->GetName());
		Entry->SetBoolField(TEXT("is_event"), false);
		FunctionsCreated.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Also detect new event nodes added to ubergraph pages (void interface functions)
	for (const UEdGraph* G : BP->UbergraphPages)
	{
		if (!G) continue;
		for (const UEdGraphNode* Node : G->Nodes)
		{
			if (const UK2Node_Event* EvNode = Cast<UK2Node_Event>(Node))
			{
				// Interface events added by ImplementNewInterface will be overrides
				if (EvNode->bOverrideFunction)
				{
					// Check that this event's function is from the interface we just added
					if (EvNode->EventReference.GetMemberParentClass() == InterfaceClass ||
						InterfaceClass->FindFunctionByName(EvNode->EventReference.GetMemberName()))
					{
						// Was this node in the ubergraph before? We don't have per-node snapshot,
						// so report all override events belonging to this interface.
						// When already_implemented=true, we still list them so the caller knows what's there.
						TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("name"), EvNode->EventReference.GetMemberName().ToString());
						Entry->SetStringField(TEXT("graph_name"), G->GetName());
						Entry->SetBoolField(TEXT("is_event"), true);
						FunctionsCreated.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("interface_name"), InterfaceClass->GetName());
	Root->SetArrayField(TEXT("functions_created"), FunctionsCreated);
	Root->SetBoolField(TEXT("already_implemented"), bAlreadyImplemented);
	if (FunctionsCreated.Num() == 0 && !bAlreadyImplemented)
	{
		Root->SetStringField(TEXT("note"),
			TEXT("No Blueprint-overridable functions found on this interface. "
			     "C++ interfaces with only native functions cannot generate stubs — override them in C++ instead."));
	}
	return FMonolithActionResult::Success(Root);
}

// --- reparent_blueprint ---

FMonolithActionResult FMonolithBlueprintGraphActions::HandleReparentBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString ClassName = Params->GetStringField(TEXT("new_parent_class"));
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: new_parent_class"));
	}

	UClass* NewParent = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NewParent)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Parent class not found: %s"), *ClassName));
	}

	if (NewParent->HasAnyClassFlags(CLASS_Interface))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot reparent to an interface class: %s"), *ClassName));
	}

	FString OldParent = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

	UBlueprintEditorLibrary::ReparentBlueprint(BP, NewParent);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("old_parent_class"), OldParent);
	Root->SetStringField(TEXT("new_parent_class"), NewParent->GetName());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  remove_event_dispatcher  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphActions::HandleRemoveEventDispatcher(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName = Params->GetStringField(TEXT("dispatcher_name"));
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: dispatcher_name"));
	}

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		if (DisplayName == DispatcherName)
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Event dispatcher not found: %s"), *DispatcherName));
	}

	// Warn if any CreateDelegate nodes still reference this dispatcher
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	TArray<FString> Warnings;

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (UK2Node_CreateDelegate* CreateDel = Cast<UK2Node_CreateDelegate>(Node))
			{
				if (CreateDel->GetDelegateSignature() &&
					CreateDel->GetDelegateSignature()->GetOuter() == SigGraph)
				{
					Warnings.Add(FString::Printf(TEXT("CreateDelegate node '%s' in graph '%s' still references this dispatcher"),
						*Node->GetName(), *Graph->GetName()));
				}
			}
			else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				if (CallNode->FunctionReference.GetMemberName() == FName(*DispatcherName))
				{
					Warnings.Add(FString::Printf(TEXT("CallFunction node '%s' in graph '%s' may reference this dispatcher"),
						*Node->GetName(), *Graph->GetName()));
				}
			}
		}
	}

	// Remove the delegate member variable alongside the graph, matching SMyBlueprint::OnDeleteDelegate.
	// The variable exists for dispatchers created in-editor (and via add_event_dispatcher as of this
	// fix); leaving it behind orphans a PC_MCDelegate variable on the Blueprint.
	FBlueprintEditorUtils::RemoveMemberVariable(BP, SigGraph->GetFName());
	FBlueprintEditorUtils::RemoveGraph(BP, SigGraph, EGraphRemoveFlags::Recompile);

	for (TObjectIterator<UK2Node_CreateDelegate> It(RF_ClassDefaultObject, /*bIncludeDerivedClasses*/ true, /*InternalExcludeFlags*/ EInternalObjectFlags::Garbage); It; ++It)
	{
		if (IsValid(*It) && IsValid(It->GetGraph()))
		{
			It->HandleAnyChange();
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed_dispatcher"), DispatcherName);

	TArray<TSharedPtr<FJsonValue>> WarnArr;
	for (const FString& W : Warnings)
	{
		WarnArr.Add(MakeShared<FJsonValueString>(W));
	}
	Root->SetArrayField(TEXT("warnings"), WarnArr);
	Root->SetNumberField(TEXT("warning_count"), WarnArr.Num());
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  set_event_dispatcher_params  (Wave 6)
// ============================================================

FMonolithActionResult FMonolithBlueprintGraphActions::HandleSetEventDispatcherParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString DispatcherName = Params->GetStringField(TEXT("dispatcher_name"));
	if (DispatcherName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: dispatcher_name"));
	}

	const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("params"), ParamsArray) || !ParamsArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: params (array of {name, type})"));
	}

	// Find the delegate signature graph
	UEdGraph* SigGraph = nullptr;
	for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
	{
		if (!Graph) continue;
		FString DisplayName = Graph->GetName();
		if (DisplayName.EndsWith(TEXT("_Signature")))
		{
			DisplayName.LeftChopInline(10, EAllowShrinking::No);
		}
		if (DisplayName == DispatcherName)
		{
			SigGraph = Graph;
			break;
		}
	}

	if (!SigGraph)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Event dispatcher not found: %s"), *DispatcherName));
	}

	// Find the FunctionEntry node in the signature graph
	UK2Node_FunctionEntry* EntryNode = nullptr;
	for (UEdGraphNode* Node : SigGraph->Nodes)
	{
		EntryNode = Cast<UK2Node_FunctionEntry>(Node);
		if (EntryNode) break;
	}

	if (!EntryNode)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No FunctionEntry node found in dispatcher signature graph: %s"), *DispatcherName));
	}

	// Parse EVERY requested param type BEFORE touching the node. This replaces the
	// existing signature wholesale, so a type error discovered mid-loop would leave
	// the dispatcher with its old pins already stripped and no replacements.
	// Strict parse — a typo'd type used to fall back to a silent bool pin.
	TArray<TPair<FName, FEdGraphPinType>> NewParams;
	for (const TSharedPtr<FJsonValue>& ParamVal : *ParamsArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!ParamVal->TryGetObject(ParamObj) || !ParamObj) continue;

		FString PinName, TypeStr;
		(*ParamObj)->TryGetStringField(TEXT("name"), PinName);
		(*ParamObj)->TryGetStringField(TEXT("type"), TypeStr);
		if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;

		FEdGraphPinType PinType;
		FString TypeError;
		if (!MonolithPinTypeGrammar::TryParsePinType(TypeStr, PinType, TypeError))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Parameter '%s' has an invalid type '%s': %s"), *PinName, *TypeStr, *TypeError));
		}
		NewParams.Emplace(FName(*PinName), MoveTemp(PinType));
	}

	// Clear existing user-defined pins safely — iterate a copy since removal mutates the array
	TArray<TSharedPtr<FUserPinInfo>> PinsToRemove = EntryNode->UserDefinedPins;
	for (const TSharedPtr<FUserPinInfo>& PinInfo : PinsToRemove)
	{
		if (PinInfo.IsValid())
		{
			EntryNode->RemoveUserDefinedPin(PinInfo);
		}
	}

	// Add new params
	int32 ParamsAdded = 0;
	for (const TPair<FName, FEdGraphPinType>& Param : NewParams)
	{
		EntryNode->CreateUserDefinedPin(Param.Key, Param.Value, EGPD_Output);
		++ParamsAdded;
	}

	// Reconstruct the node to apply pin changes
	EntryNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("dispatcher_name"), DispatcherName);
	Root->SetNumberField(TEXT("params_set"), ParamsAdded);
	return FMonolithActionResult::Success(Root);
}
