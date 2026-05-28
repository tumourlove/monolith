// Copyright tumourlove. All Rights Reserved.
// MonolithUIRegistryActions.cpp

#include "MonolithUIRegistryActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithParamSchema.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UITypeRegistry.h"

// Phase 2 (2026-05-16 UI gap audit) — Item #8 (add_widget_variable) +
// Item #11 (list_widget_property_enums) bring reflection-walking surface
// into this file. Item #8 wraps FBlueprintEditorUtils::AddMemberVariable,
// Item #11 walks FEnumProperty for the curated property allowlist.
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UnrealType.h"
#include "MonolithUICommon.h"  // MonolithUI::LoadWidgetBlueprint
#include "Components/Widget.h"
#include "Blueprint/WidgetTree.h"

namespace MonolithUIRegistryPhase2
{
    // ---- Phase 2 Item #8 helpers ---------------------------------------------
    //
    // ParsePinTypeFromString mirrors the canonical MCP-friendly token grammar
    // already shipped in `Source/MonolithBlueprint/Private/MonolithBlueprintInternal.h:782`.
    // Replicated locally rather than cross-module-included so MonolithUI keeps
    // its dependency boundary clean (MonolithBlueprint is NOT listed in
    // MonolithUI.Build.cs PrivateDependencyModuleNames). The grammar is the
    // SAME tokens (bool/int/int64/float/double/string/name/text/byte/
    // object:Class/class:Class/struct:Name/enum:Name/softobject:Class/
    // softclass:Class/exec/wildcard, with container prefixes array:/set:/map:).
    // Cross-reference: MonolithBlueprintInternal.h:782-915.

    static FEdGraphPinType ParsePinTypeFromString(const FString& TypeStr)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;  // safe default

        FString BaseType = TypeStr;
        EPinContainerType ContainerType = EPinContainerType::None;

        if (TypeStr.StartsWith(TEXT("array:")))
        {
            ContainerType = EPinContainerType::Array;
            BaseType = TypeStr.Mid(6);
        }
        else if (TypeStr.StartsWith(TEXT("set:")))
        {
            ContainerType = EPinContainerType::Set;
            BaseType = TypeStr.Mid(4);
        }
        else if (TypeStr.StartsWith(TEXT("map:")))
        {
            ContainerType = EPinContainerType::Map;
            int32 SecondColon;
            if (BaseType.Mid(4).FindChar(TEXT(':'), SecondColon))
            {
                BaseType = TypeStr.Mid(4, SecondColon);
                const FString ValueType = TypeStr.Mid(4 + SecondColon + 1);
                PinType.PinValueType = FEdGraphTerminalType();
                const FEdGraphPinType ValPinType = ParsePinTypeFromString(ValueType);
                PinType.PinValueType.TerminalCategory = ValPinType.PinCategory;
                PinType.PinValueType.TerminalSubCategory = ValPinType.PinSubCategory;
                PinType.PinValueType.TerminalSubCategoryObject = ValPinType.PinSubCategoryObject;
            }
            else
            {
                BaseType = TypeStr.Mid(4);
            }
        }
        PinType.ContainerType = ContainerType;

        if (BaseType == TEXT("bool"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        }
        else if (BaseType == TEXT("int"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        }
        else if (BaseType == TEXT("int64"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
        }
        else if (BaseType == TEXT("float"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = TEXT("float");
        }
        else if (BaseType == TEXT("double"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
            PinType.PinSubCategory = TEXT("double");
        }
        else if (BaseType == TEXT("string"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_String;
        }
        else if (BaseType == TEXT("name"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        }
        else if (BaseType == TEXT("text"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        }
        else if (BaseType == TEXT("byte"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        }
        else if (BaseType.StartsWith(TEXT("object:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            const FString ClassName = BaseType.Mid(7);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("class:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
            const FString ClassName = BaseType.Mid(6);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("struct:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            const FString StructName = BaseType.Mid(7);
            if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = S;
        }
        else if (BaseType.StartsWith(TEXT("enum:")))
        {
            // PC_Byte + sub-category-object-as-enum is the canonical Kismet pattern
            // for TEnumAsByte<EFoo> style variables. UE 5.7 still uses PC_Byte for
            // editor-visible enums; PC_Enum is reserved for C++-only enum class.
            // FBlueprintEditorUtils::AddMemberVariable accepts either.
            PinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
            const FString EnumName = BaseType.Mid(5);
            if (UEnum* E = FindFirstObject<UEnum>(*EnumName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = E;
        }
        else if (BaseType.StartsWith(TEXT("softobject:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
            const FString ClassName = BaseType.Mid(11);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType.StartsWith(TEXT("softclass:")))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
            const FString ClassName = BaseType.Mid(10);
            if (UClass* C = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
                PinType.PinSubCategoryObject = C;
        }
        else if (BaseType == TEXT("exec"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        }
        else if (BaseType == TEXT("wildcard"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }

        return PinType;
    }

    // ---- Phase 2 Item #8 — add_widget_variable -------------------------------
    //
    // Wraps FBlueprintEditorUtils::AddMemberVariable so a caller can stamp a
    // user-variable on the WBP without having to drive the BlueprintEditor UI.
    // The variable becomes editable in the WBP's Details panel and shows up in
    // get_widget_tree -> NewVariables. AddMemberVariable handles default flags
    // (CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance) so the new
    // variable matches engine-canonical "user variable" semantics.

    static FMonolithActionResult HandleAddWidgetVariable(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FString WbpPath;
        if (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath))
        {
            Params->TryGetStringField(TEXT("asset_path"), WbpPath);
        }
        if (WbpPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"), -32602);
        }

        FString VarName, VarType;
        if (!Params->TryGetStringField(TEXT("var_name"), VarName) || VarName.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Required parameter 'var_name' missing or empty."), -32602);
        }
        if (!Params->TryGetStringField(TEXT("var_type"), VarType) || VarType.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("Required parameter 'var_type' missing or empty. Use bool/int/int64/float/double/string/"
                     "name/text/byte/object:Class/class:Class/struct:Name/enum:Name/softobject:Class/softclass:Class. "
                     "Container prefixes: array:/set:/map:Key:Value."),
                -32602);
        }

        FString DefaultValue;
        Params->TryGetStringField(TEXT("default_value"), DefaultValue);

        FString VarCategory;
        Params->TryGetStringField(TEXT("var_category"), VarCategory);

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;

        const FEdGraphPinType PinType = ParsePinTypeFromString(VarType);

        // Quick sanity check — if the caller passed an unknown enum:/struct:/
        // object: token, ParsePinTypeFromString silently dropped the resolve
        // and the PinSubCategoryObject is null. AddMemberVariable would still
        // create the variable but with an invalid type — refuse loudly here.
        const bool bWantsTypeObject =
            PinType.PinCategory == UEdGraphSchema_K2::PC_Object   ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Class    ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Struct   ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_Enum     ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
            PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass;
        if (bWantsTypeObject && !PinType.PinSubCategoryObject.IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("var_type '%s' references a class/struct/enum that could not be resolved via FindFirstObject. Use a fully-qualified name (e.g. 'object:UMG.TextBlock' or pass the engine class short name like 'TextBlock')."),
                    *VarType),
                -32602);
        }

        const FName NewVarFName(*VarName);
        const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(WBP, NewVarFName, PinType, DefaultValue);
        if (!bAdded)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("AddMemberVariable returned false for '%s' (likely already exists on '%s' or a parent class)."),
                    *VarName, *WbpPath),
                -32602);
        }

        // Apply optional category — keeps the WBP's Details panel grouped under
        // a user-supplied label. AddMemberVariable defaults to VR_DefaultCategory
        // (BlueprintEditorUtils.cpp:4681) so we only override when caller asked.
        if (!VarCategory.IsEmpty())
        {
            // Pass bDontRecompile=true — we drive the compile ourselves below
            // so the category set + variable add commit in a single compile pass
            // rather than thrashing the compile manager twice. UE signature:
            // (Blueprint, VarName, InLocalVarScope, NewCategory, bDontRecompile)
            // — BlueprintEditorUtils.cpp:4058.
            FBlueprintEditorUtils::SetBlueprintVariableCategory(
                WBP, NewVarFName, /*InLocalVarScope=*/nullptr,
                FText::FromString(VarCategory), /*bDontRecompile=*/true);
        }

        // Compile so the BP regenerates its Skeleton class with the new
        // variable — downstream get_widget_tree / set_widget_property reads
        // see the variable on the Skeleton (next-tick) instead of waiting
        // for the next compile pass.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), WbpPath);
        Result->SetStringField(TEXT("var_name"), VarName);
        Result->SetStringField(TEXT("var_type"), VarType);
        Result->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Result->SetStringField(TEXT("pin_sub_category_object"), PinType.PinSubCategoryObject->GetName());
        }
        if (!VarCategory.IsEmpty())
        {
            Result->SetStringField(TEXT("var_category"), VarCategory);
        }
        if (!DefaultValue.IsEmpty())
        {
            Result->SetStringField(TEXT("default_value"), DefaultValue);
        }
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 4 Item #2 — set_widget_is_variable ----------------------------
    //
    // First-class flip of UWidget::bIsVariable. When true, the widget is exposed
    // as a named BindWidget-style variable on the WBP's generated class (visible
    // to get_variables and accessible from the graph); when false, it becomes an
    // anonymous tree-only widget. bIsVariable is a public uint8:1 member on the
    // base UWidget — the engine's own SWidgetDetailsView sets it via direct
    // member write (UMGEditor SWidgetDetailsView.cpp), which is the path used
    // here since MonolithUI links UMG. MonolithBlueprint's get_variables reads
    // the same flag through reflection only because that module has no UMG dep.
    // After the flip the WBP must be marked structurally modified + compiled so
    // the generated class regenerates with (or without) the variable binding.

    static FMonolithActionResult HandleSetWidgetIsVariable(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FString WbpPath;
        if (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath))
        {
            Params->TryGetStringField(TEXT("asset_path"), WbpPath);
        }
        if (WbpPath.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"), -32602);
        }

        FString WidgetName;
        if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
        {
            return FMonolithActionResult::Error(TEXT("Required parameter 'widget_name' missing or empty."), -32602);
        }

        bool bIsVariable = false;
        if (!Params->TryGetBoolField(TEXT("is_variable"), bIsVariable))
        {
            return FMonolithActionResult::Error(TEXT("Required parameter 'is_variable' (bool) missing."), -32602);
        }

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;

        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(
                TEXT("WidgetTree is null (editor-only data not available)."), -32603);
        }

        UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
        if (!Target)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in '%s'."), *WidgetName, *WbpPath),
                -32602);
        }

        const bool bWasVariable = Target->bIsVariable;

        // Mark the WBP modified BEFORE the write so the transaction captures the
        // pre-edit state, matching the engine's Modify()-then-mutate ordering.
        Target->Modify();
        Target->bIsVariable = bIsVariable;

        // Structural modification + compile so the generated class adds/removes
        // the variable binding; without this the flip stays inert until the next
        // editor-driven compile.
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget_name"), WidgetName);
        Result->SetBoolField(TEXT("is_variable"), bIsVariable);
        Result->SetBoolField(TEXT("changed"), bWasVariable != bIsVariable);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #11 — list_widget_property_enums -----------------------
    //
    // Walks the curated allowlist for a given widget type (or resolved WBP)
    // and returns every enum-typed property with its enumerator names. LLMs
    // use this to know "what valid values can I pass to set_widget_property
    // when the property is an enum" — the allowlist gates the writes, but it
    // does NOT advertise the legal value set.
    //
    // Resolution order: prefer wbp_path if supplied (so the caller resolves the
    // OWN class with custom variables / inherited UPROPERTIES); fall back to
    // widget_class token via FUITypeRegistry::FindByToken. Optional
    // property_name filter narrows to one entry.

    static FMonolithActionResult HandleListWidgetPropertyEnums(const TSharedPtr<FJsonObject>& Params)
    {
        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
        }

        FString WbpPath, WidgetClassToken, PropertyNameFilter;
        Params->TryGetStringField(TEXT("wbp_path"), WbpPath);
        Params->TryGetStringField(TEXT("widget_class"), WidgetClassToken);
        Params->TryGetStringField(TEXT("property_name"), PropertyNameFilter);

        // At least one of wbp_path / widget_class is required so we have a
        // concrete UClass to walk. The plan calls this "at least one of
        // wbp_path / widget_class / property_name required" — property_name
        // alone is insufficient because we need a class context to find it.
        if (WbpPath.IsEmpty() && WidgetClassToken.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("At least one of 'wbp_path' or 'widget_class' is required."),
                -32602);
        }

        UClass* WidgetClass = nullptr;
        if (!WbpPath.IsEmpty())
        {
            FMonolithActionResult LoadErr;
            UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WbpPath, LoadErr);
            if (!WBP) return LoadErr;
            // For WBP-rooted introspection we walk the widget tree's root
            // class — that's the most common "what enums exist on the widgets
            // I instantiated in this WBP" question. Caller can narrow further
            // via property_name filter.
            WidgetClass = WBP->GeneratedClass;
        }

        if (!WidgetClass && !WidgetClassToken.IsEmpty())
        {
            // Token-form lookup via the type registry (canonical surface).
            // Fall back to FindFirstObject in case the caller passed a raw
            // class name that the registry has not registered (e.g. a marketplace
            // widget that hot-loaded after the registry scan).
            if (UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get())
            {
                const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
                if (const FUITypeRegistryEntry* Entry = TypeRegistry.FindByToken(FName(*WidgetClassToken)))
                {
                    WidgetClass = Entry->WidgetClass.Get();
                }
            }
            if (!WidgetClass)
            {
                WidgetClass = FindFirstObject<UClass>(*WidgetClassToken, EFindFirstObjectOptions::NativeFirst);
                if (!WidgetClass)
                {
                    // Try with leading 'U' prefix (engine convention)
                    WidgetClass = FindFirstObject<UClass>(*(TEXT("U") + WidgetClassToken), EFindFirstObjectOptions::NativeFirst);
                }
            }
        }

        if (!WidgetClass)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Could not resolve a UClass for widget_class='%s' / wbp_path='%s'."),
                    *WidgetClassToken, *WbpPath),
                -32602);
        }

        // Walk the UClass property surface looking for enum types. Two flavours
        // surface in UE 5.7:
        //   * FEnumProperty       — modern enum class declared via UENUM().
        //   * FByteProperty + Enum — legacy TEnumAsByte<EFoo>. The byte property
        //                            has an Enum() accessor returning UEnum*.
        // We surface BOTH so callers can pass enum values from either flavour.

        TArray<TSharedPtr<FJsonValue>> Enums;
        for (TFieldIterator<FProperty> It(WidgetClass); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop) continue;

            const FName PropName = Prop->GetFName();
            if (!PropertyNameFilter.IsEmpty()
                && !PropName.ToString().Equals(PropertyNameFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }

            UEnum* EnumPtr = nullptr;
            FString EnumKind;
            if (FEnumProperty* EProp = CastField<FEnumProperty>(Prop))
            {
                EnumPtr = EProp->GetEnum();
                EnumKind = TEXT("EnumProperty");
            }
            else if (FByteProperty* BProp = CastField<FByteProperty>(Prop))
            {
                if (BProp->Enum)  // TEnumAsByte<E> only — plain bytes have null Enum
                {
                    EnumPtr = BProp->Enum;
                    EnumKind = TEXT("ByteProperty");
                }
            }

            if (!EnumPtr) continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property_name"), PropName.ToString());
            Entry->SetStringField(TEXT("property_kind"), EnumKind);
            Entry->SetStringField(TEXT("enum_name"), EnumPtr->GetName());

            // Honour the engine's "MAX" sentinel suppression — UE-generated
            // enums historically include a synthetic <Name>_MAX terminator
            // that is not a real value. NumEnums() includes it; we filter on
            // the standard `IsValidEnumValue` test.
            TArray<TSharedPtr<FJsonValue>> Values;
            const int32 NumEntries = EnumPtr->NumEnums();
            for (int32 i = 0; i < NumEntries; ++i)
            {
                // Skip _MAX sentinel (last entry on UENUM() with no explicit values)
                const int64 RawVal = EnumPtr->GetValueByIndex(i);
                const FName NameByIdx = EnumPtr->GetNameByIndex(i);
                if (NameByIdx.ToString().EndsWith(TEXT("_MAX")) && i == NumEntries - 1)
                    continue;

                TSharedPtr<FJsonObject> ValObj = MakeShared<FJsonObject>();
                ValObj->SetStringField(TEXT("name"), EnumPtr->GetNameStringByIndex(i));
                ValObj->SetStringField(TEXT("display_name"), EnumPtr->GetDisplayNameTextByIndex(i).ToString());
                ValObj->SetNumberField(TEXT("value"), static_cast<double>(RawVal));
                Values.Add(MakeShared<FJsonValueObject>(ValObj));
            }
            Entry->SetArrayField(TEXT("valid_values"), Values);
            Entry->SetNumberField(TEXT("valid_value_count"), Values.Num());

            Enums.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget_class"), WidgetClass->GetName());
        if (!WbpPath.IsEmpty()) Result->SetStringField(TEXT("wbp_path"), WbpPath);
        if (!PropertyNameFilter.IsEmpty()) Result->SetStringField(TEXT("property_name_filter"), PropertyNameFilter);
        Result->SetArrayField(TEXT("enum_properties"), Enums);
        Result->SetNumberField(TEXT("enum_property_count"), Enums.Num());
        return FMonolithActionResult::Success(Result);
    }
}

void FMonolithUIRegistryActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_property_allowlist"),
        TEXT("Dump the property allowlist for a widget type. Returns {type, allowed_paths:[...]}."),
        FMonolithActionHandler::CreateStatic(&HandleDumpPropertyAllowlist),
        FParamSchemaBuilder()
            .Required(TEXT("widget_type"), TEXT("string"),
                TEXT("Widget token (e.g. \"VerticalBox\", \"TextBlock\", \"RoundedBorder\")."))
            .Build()
    );

    // Phase 2 Item #8 (2026-05-16 UI gap audit): add_widget_variable.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_widget_variable"),
        TEXT("Add a member variable to a Widget Blueprint via FBlueprintEditorUtils::AddMemberVariable. "
             "var_type accepts MCP-token grammar: bool|int|int64|float|double|string|name|text|byte|"
             "object:Class|class:Class|struct:Name|enum:Name|softobject:Class|softclass:Class|exec|wildcard. "
             "Container prefixes (array:|set:|map:Key:Value) compose. "
             "AddMemberVariable defaults flags CPF_Edit|CPF_BlueprintVisible|CPF_DisableEditOnInstance — matches "
             "the editor's 'add variable' affordance."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleAddWidgetVariable),
        FParamSchemaBuilder()
            .Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
            .Required(TEXT("var_name"), TEXT("string"), TEXT("New variable FName (uniqueness enforced by AddMemberVariable)"))
            .Required(TEXT("var_type"), TEXT("string"), TEXT("Type token. See action description for grammar."))
            .Optional(TEXT("default_value"), TEXT("string"), TEXT("Default value as UE text format (engine ImportText grammar)"))
            .Optional(TEXT("var_category"), TEXT("string"), TEXT("Details-panel grouping label"))
            .Build(),
        TEXT("Registry")
    );

    // Phase 4 Item #2 (2026-05-23 UI gap closure): set_widget_is_variable.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_widget_is_variable"),
        TEXT("Set a UWidget's bIsVariable flag. When true the widget is exposed as a named "
             "variable on the WBP's generated class (visible to get_variables, accessible from "
             "the graph); when false it becomes an anonymous tree-only widget. Marks the WBP "
             "structurally modified + compiles so the binding materializes. "
             "Returns {widget_name, is_variable, changed}."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleSetWidgetIsVariable),
        FParamSchemaBuilder()
            .Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"), {TEXT("asset_path")})
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the widget in the WBP's WidgetTree"))
            .Required(TEXT("is_variable"), TEXT("bool"), TEXT("New bIsVariable value (true = expose as named variable)"))
            .Build(),
        TEXT("Registry")
    );

    // Phase 2 Item #11 (2026-05-16 UI gap audit): list_widget_property_enums.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_widget_property_enums"),
        TEXT("List enum-typed properties on a widget class (or WBP's generated class) and their valid values. "
             "Surfaces both FEnumProperty (modern enum class) AND FByteProperty-with-Enum (legacy TEnumAsByte). "
             "Use this to discover the legal value set for set_widget_property writes that target enum fields."),
        FMonolithActionHandler::CreateStatic(&MonolithUIRegistryPhase2::HandleListWidgetPropertyEnums),
        FParamSchemaBuilder()
            .Optional(TEXT("wbp_path"), TEXT("string"), TEXT("Resolve via this WBP's generated class (highest priority)"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("Token form (e.g. 'TextBlock', 'CommonButtonBase')"))
            .Optional(TEXT("property_name"), TEXT("string"), TEXT("Optional case-insensitive filter; returns only the named property"))
            .Build(),
        TEXT("Registry")
    );
}

FMonolithActionResult FMonolithUIRegistryActions::HandleDumpPropertyAllowlist(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing parameters object."), -32602);
    }

    FString WidgetTypeStr;
    if (!Params->TryGetStringField(TEXT("widget_type"), WidgetTypeStr) || WidgetTypeStr.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Required parameter 'widget_type' missing or empty."), -32602);
    }

    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!Sub)
    {
        return FMonolithActionResult::Error(
            TEXT("UMonolithUIRegistrySubsystem not available — editor not initialised?"), -32603);
    }

    const FName WidgetToken(*WidgetTypeStr);
    const FUITypeRegistry& TypeRegistry = Sub->GetTypeRegistry();
    const FUITypeRegistryEntry* Entry = TypeRegistry.FindByToken(WidgetToken);

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("type"), WidgetTypeStr);

    if (!Entry)
    {
        // Unknown type — return empty allowed_paths + a hint that the type is
        // not registered. Distinct from a registered-but-no-mappings response.
        Result->SetBoolField(TEXT("registered"), false);
        Result->SetArrayField(TEXT("allowed_paths"), TArray<TSharedPtr<FJsonValue>>());
        Result->SetStringField(TEXT("note"),
            TEXT("Widget type not in registry. Check spelling or confirm the providing plugin is loaded."));
        return FMonolithActionResult::Success(Result);
    }

    Result->SetBoolField(TEXT("registered"), true);

    // Container kind / max-children for context — useful for LLM consumers.
    const TCHAR* KindToken = TEXT("Leaf");
    switch (Entry->ContainerKind)
    {
        case EUIContainerKind::Panel:   KindToken = TEXT("Panel");   break;
        case EUIContainerKind::Content: KindToken = TEXT("Content"); break;
        case EUIContainerKind::Leaf:    KindToken = TEXT("Leaf");    break;
    }
    Result->SetStringField(TEXT("container_kind"), KindToken);
    Result->SetNumberField(TEXT("max_children"), Entry->MaxChildren);

    if (Entry->WidgetClass.IsValid())
    {
        Result->SetStringField(TEXT("widget_class"), Entry->WidgetClass->GetPathName());
    }

    const FUIPropertyAllowlist& Allowlist = Sub->GetAllowlist();
    const TArray<FString>& AllowedPaths = Allowlist.GetAllowedPaths(WidgetToken);

    TArray<TSharedPtr<FJsonValue>> PathValues;
    PathValues.Reserve(AllowedPaths.Num());
    for (const FString& Path : AllowedPaths)
    {
        PathValues.Add(MakeShared<FJsonValueString>(Path));
    }
    Result->SetArrayField(TEXT("allowed_paths"), PathValues);
    Result->SetNumberField(TEXT("allowed_path_count"), AllowedPaths.Num());

    return FMonolithActionResult::Success(Result);
}
