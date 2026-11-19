// Copyright tumourlove. All Rights Reserved.
#include "MVVM/MonolithUIMVVMActions.h"

#if WITH_MVVM

#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "Editor.h"
#include "ScopedTransaction.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "INotifyFieldValueChanged.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// ModelViewViewModelEditor
#include "MVVMEditorSubsystem.h"

// ModelViewViewModelBlueprint
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMBlueprintInstancedViewModel.h"
#include "MVVMBlueprintPin.h"
#include "MVVMBlueprintFunctionReference.h"
#include "MVVMPropertyPath.h"

// ModelViewViewModel (runtime)
#include "Types/MVVMBindingMode.h"
#include "Types/MVVMExecutionMode.h"
#include "Types/MVVMConditionOperation.h"
#include "Types/MVVMFieldVariant.h"
#include "View/MVVMViewModelContextResolver.h"

// All file-local helpers carry the MvvmActions prefix: MonolithUI is built in
// full unity for releases, so anonymous-namespace names must be unique across
// the whole module.
namespace
{
    // ------------------------------------------------------------------
    // Common plumbing
    // ------------------------------------------------------------------

    UMVVMEditorSubsystem* MvvmActionsGetSubsystem(FMonolithActionResult& OutError)
    {
        UMVVMEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
        if (!Subsystem)
        {
            OutError = FMonolithActionResult::Error(
                TEXT("UMVVMEditorSubsystem is unavailable. Enable the 'UMG Viewmodel' (ModelViewViewModel) plugin and restart the editor."));
        }
        return Subsystem;
    }

    /**
     * Set the private FMVVMBlueprintViewModelContext::bCreateSetterFunction_Deprecation
     * flag through reflection. Contexts built at runtime leave it false, so the struct's
     * PostSerialize treats them as pre-upgrade assets on the next load (including
     * transaction undo/redo) and forces bOptional/bCreateSetterFunction to true for
     * Manual contexts. The struct type is reached through the view's AvailableViewModels
     * property because the USTRUCT is not exported from its module.
     */
    bool MvvmActionsMarkContextUpgraded(const UMVVMBlueprintView* View, FMVVMBlueprintViewModelContext& Ctx)
    {
        const FArrayProperty* ArrayProp = View ? FindFProperty<FArrayProperty>(View->GetClass(), TEXT("AvailableViewModels")) : nullptr;
        const FStructProperty* InnerProp = ArrayProp ? CastField<FStructProperty>(ArrayProp->Inner) : nullptr;
        const FBoolProperty* FlagProp = (InnerProp && InnerProp->Struct)
            ? FindFProperty<FBoolProperty>(InnerProp->Struct, TEXT("bCreateSetterFunction_Deprecation"))
            : nullptr;
        if (!FlagProp)
        {
            return false;
        }
        FlagProp->SetPropertyValue_InContainer(&Ctx, true);
        return true;
    }

    bool MvvmActionsRefuseDuringPIE(FMonolithActionResult& OutError)
    {
        if (GEditor && GEditor->PlayWorld)
        {
            OutError = FMonolithActionResult::Error(
                TEXT("MVVM edits are refused while a Play-In-Editor session is running. Stop PIE and retry."));
            return true;
        }
        return false;
    }

    const UClass* MvvmActionsSelfContext(const UWidgetBlueprint* WBP)
    {
        if (!WBP)
        {
            return nullptr;
        }
        return WBP->SkeletonGeneratedClass ? WBP->SkeletonGeneratedClass.Get() : WBP->GeneratedClass.Get();
    }

    FString MvvmActionsGuidToString(const FGuid& Guid)
    {
        return Guid.ToString(EGuidFormats::DigitsWithHyphens);
    }

    TArray<TSharedPtr<FJsonValue>> MvvmActionsTextsToJson(const TArray<FText>& Texts)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FText& Text : Texts)
        {
            Out.Add(MakeShared<FJsonValueString>(Text.ToString()));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> MvvmActionsStringsToJson(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& S : Strings)
        {
            Out.Add(MakeShared<FJsonValueString>(S));
        }
        return Out;
    }

    // UENUM name <-> value. Iterates the reflected enum so engine-version
    // differences (e.g. the 'Context' creation type added in UE 5.8) are
    // picked up without version gates.
    template <typename EnumType>
    FString MvvmActionsEnumToString(EnumType Value)
    {
        if (const UEnum* Enum = StaticEnum<EnumType>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Value));
        }
        return FString::FromInt(static_cast<int32>(Value));
    }

    template <typename EnumType>
    TArray<FString> MvvmActionsEnumNames()
    {
        TArray<FString> Names;
        if (const UEnum* Enum = StaticEnum<EnumType>())
        {
            for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
            {
                const FString Name = Enum->GetNameStringByIndex(Index);
                if (!Name.EndsWith(TEXT("_MAX")))
                {
                    Names.Add(Name);
                }
            }
        }
        return Names;
    }

    template <typename EnumType>
    bool MvvmActionsEnumFromString(const FString& In, EnumType& OutValue)
    {
        if (const UEnum* Enum = StaticEnum<EnumType>())
        {
            for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
            {
                const FString Name = Enum->GetNameStringByIndex(Index);
                if (!Name.EndsWith(TEXT("_MAX")) && Name.Equals(In.TrimStartAndEnd(), ESearchCase::IgnoreCase))
                {
                    OutValue = static_cast<EnumType>(Enum->GetValueByIndex(Index));
                    return true;
                }
            }
        }
        return false;
    }

    void MvvmActionsFinish(UWidgetBlueprint* WBP, bool bStructural, bool bCompile)
    {
        if (bStructural)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        }
        else
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        }
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
        }
    }

    // ------------------------------------------------------------------
    // Serialisation
    // ------------------------------------------------------------------

    /**
     * Serialise a property path. `path` is written in the same grammar the
     * write actions accept: "viewmodel:<Name>.<Field>...", "widget:<Name>.<Field>...",
     * or "self:<Field>...".
     */
    TSharedPtr<FJsonObject> MvvmActionsPathToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const FMVVMBlueprintPropertyPath& Path)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("valid"), Path.IsValid());

        FString Kind = TEXT("none");
        FString Owner;
        switch (Path.GetSource(WBP))
        {
        case EMVVMBlueprintFieldPathSource::ViewModel:
            Kind = TEXT("viewmodel");
            if (const FMVVMBlueprintViewModelContext* Ctx = View ? View->FindViewModel(Path.GetViewModelId()) : nullptr)
            {
                Owner = Ctx->GetViewModelName().ToString();
            }
            Obj->SetStringField(TEXT("viewmodel_id"), MvvmActionsGuidToString(Path.GetViewModelId()));
            break;
        case EMVVMBlueprintFieldPathSource::Widget:
            Kind = TEXT("widget");
            Owner = Path.GetWidgetName().ToString();
            break;
        case EMVVMBlueprintFieldPathSource::SelfContext:
            Kind = TEXT("self");
            Owner = WBP ? WBP->GetName() : FString();
            break;
        default:
            break;
        }
        Obj->SetStringField(TEXT("kind"), Kind);
        Obj->SetStringField(TEXT("owner"), Owner);

        TArray<FString> FieldNames;
        for (const FName& Name : Path.GetFieldNames(MvvmActionsSelfContext(WBP)))
        {
            FieldNames.Add(Name.ToString());
        }
        const FString Fields = FString::Join(FieldNames, TEXT("."));
        Obj->SetArrayField(TEXT("fields"), MvvmActionsStringsToJson(FieldNames));
        Obj->SetStringField(TEXT("field_path"), Fields);

        FString Grammar;
        if (Kind == TEXT("self"))
        {
            Grammar = FString::Printf(TEXT("self:%s"), *Fields);
        }
        else if (Kind != TEXT("none"))
        {
            Grammar = Fields.IsEmpty()
                ? FString::Printf(TEXT("%s:%s"), *Kind, *Owner)
                : FString::Printf(TEXT("%s:%s.%s"), *Kind, *Owner, *Fields);
        }
        Obj->SetStringField(TEXT("path"), Grammar);
        Obj->SetBoolField(TEXT("is_component"), Path.IsComponent());
        if (WBP)
        {
            Obj->SetStringField(TEXT("display"), Path.ToString(WBP, /*bUseDisplayName*/ false, /*bIncludeMetaData*/ false));
        }
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> MvvmActionsPinsToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, TArrayView<const FMVVMBlueprintPin> Pins)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        const UClass* SelfContext = MvvmActionsSelfContext(WBP);
        for (const FMVVMBlueprintPin& Pin : Pins)
        {
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            TArray<FString> Names;
            for (const FName& Name : Pin.GetId().GetNames())
            {
                Names.Add(Name.ToString());
            }
            PinObj->SetStringField(TEXT("id"), Pin.GetId().ToString());
            PinObj->SetArrayField(TEXT("names"), MvvmActionsStringsToJson(Names));
            PinObj->SetBoolField(TEXT("split"), Pin.IsSplit());
            PinObj->SetStringField(TEXT("status"), MvvmActionsEnumToString(Pin.GetStatus()));
            PinObj->SetBoolField(TEXT("uses_path"), Pin.UsedPathAsValue());
            if (Pin.UsedPathAsValue())
            {
                PinObj->SetObjectField(TEXT("source_path"), MvvmActionsPathToJson(WBP, View, Pin.GetPath()));
            }
            PinObj->SetStringField(TEXT("value"), Pin.GetValueAsString(SelfContext));
            Out.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        return Out;
    }

    TSharedPtr<FJsonObject> MvvmActionsConversionToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const UMVVMBlueprintViewConversionFunction* Conversion)
    {
        if (!Conversion)
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const FMVVMBlueprintFunctionReference Ref = Conversion->GetConversionFunction();
        Obj->SetStringField(TEXT("type"), MvvmActionsEnumToString(Ref.GetType()));
        Obj->SetStringField(TEXT("name"), Ref.GetName().ToString());
        Obj->SetStringField(TEXT("function"), Ref.ToString());
        Obj->SetStringField(TEXT("wrapper_graph"), Conversion->GetWrapperGraphName().ToString());
        Obj->SetArrayField(TEXT("pins"), MvvmActionsPinsToJson(WBP, View, Conversion->GetPins()));
        return Obj;
    }

    TSharedPtr<FJsonObject> MvvmActionsViewModelToJson(const FMVVMBlueprintViewModelContext& Ctx)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Ctx.GetViewModelName().ToString());
        Obj->SetStringField(TEXT("id"), MvvmActionsGuidToString(Ctx.GetViewModelId()));
        Obj->SetStringField(TEXT("class"), Ctx.GetViewModelClass() ? Ctx.GetViewModelClass()->GetPathName() : FString());
        Obj->SetStringField(TEXT("creation_type"), MvvmActionsEnumToString(Ctx.CreationType));
        Obj->SetStringField(TEXT("global_identifier"), Ctx.GlobalViewModelIdentifier.ToString());
        Obj->SetStringField(TEXT("property_path"), Ctx.ViewModelPropertyPath);
        const UMVVMViewModelContextResolver* Resolver = Ctx.Resolver.Get();
        Obj->SetStringField(TEXT("resolver_class"), Resolver ? Resolver->GetClass()->GetPathName() : FString());
        Obj->SetBoolField(TEXT("instanced"), Ctx.InstancedViewModel.Get() != nullptr);
        Obj->SetBoolField(TEXT("optional"), Ctx.bOptional);
        Obj->SetBoolField(TEXT("create_setter"), Ctx.bCreateSetterFunction);
        Obj->SetBoolField(TEXT("create_getter"), Ctx.bCreateGetterFunction);
        Obj->SetBoolField(TEXT("expose_instance_in_editor"), Ctx.bExposeInstanceInEditor);
        Obj->SetBoolField(TEXT("use_as_interface"), Ctx.bUseAsInterface);
        Obj->SetBoolField(TEXT("can_rename"), Ctx.CanRename());
        Obj->SetBoolField(TEXT("can_edit"), Ctx.bCanEdit);
        Obj->SetBoolField(TEXT("can_remove"), Ctx.bCanRemove);
        return Obj;
    }

    TSharedPtr<FJsonObject> MvvmActionsBindingToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const FMVVMBlueprintViewBinding& Binding, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("id"), MvvmActionsGuidToString(Binding.BindingId));
        Obj->SetStringField(TEXT("display_name"), Binding.GetDisplayNameString(WBP));
        Obj->SetStringField(TEXT("mode"), MvvmActionsEnumToString(Binding.BindingType));
        Obj->SetBoolField(TEXT("enabled"), Binding.bEnabled);
        Obj->SetBoolField(TEXT("compile"), Binding.bCompile);
        Obj->SetBoolField(TEXT("override_execution_mode"), Binding.bOverrideExecutionMode);
        if (Binding.bOverrideExecutionMode)
        {
            Obj->SetStringField(TEXT("execution_mode"), MvvmActionsEnumToString(Binding.OverrideExecutionMode));
        }
        Obj->SetObjectField(TEXT("source"), MvvmActionsPathToJson(WBP, View, Binding.SourcePath));
        Obj->SetObjectField(TEXT("destination"), MvvmActionsPathToJson(WBP, View, Binding.DestinationPath));

        if (TSharedPtr<FJsonObject> Forward = MvvmActionsConversionToJson(WBP, View, Binding.Conversion.GetConversionFunction(true)))
        {
            Obj->SetObjectField(TEXT("conversion_source_to_destination"), Forward);
        }
        if (TSharedPtr<FJsonObject> Backward = MvvmActionsConversionToJson(WBP, View, Binding.Conversion.GetConversionFunction(false)))
        {
            Obj->SetObjectField(TEXT("conversion_destination_to_source"), Backward);
        }

        if (View)
        {
            TSharedPtr<FJsonObject> Messages = MakeShared<FJsonObject>();
            Messages->SetArrayField(TEXT("errors"), MvvmActionsTextsToJson(View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Error)));
            Messages->SetArrayField(TEXT("warnings"), MvvmActionsTextsToJson(View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Warning)));
            Messages->SetArrayField(TEXT("info"), MvvmActionsTextsToJson(View->GetBindingMessages(Binding.BindingId, UE::MVVM::EBindingMessageType::Info)));
            Obj->SetObjectField(TEXT("messages"), Messages);
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> MvvmActionsEventToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const UMVVMBlueprintViewEvent* Event, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("name"), Event->GetName());
        Obj->SetStringField(TEXT("display_name"), Event->GetDisplayName(/*bUseDisplayName*/ false).ToString());
        Obj->SetBoolField(TEXT("enabled"), Event->bEnabled);
        Obj->SetBoolField(TEXT("compile"), Event->bCompile);
        Obj->SetObjectField(TEXT("event"), MvvmActionsPathToJson(WBP, View, Event->GetEventPath()));
        Obj->SetObjectField(TEXT("destination"), MvvmActionsPathToJson(WBP, View, Event->GetDestinationPath()));
        Obj->SetArrayField(TEXT("pins"), MvvmActionsPinsToJson(WBP, View, Event->GetPins()));

        TSharedPtr<FJsonObject> Messages = MakeShared<FJsonObject>();
        Messages->SetArrayField(TEXT("errors"), MvvmActionsTextsToJson(Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Error)));
        Messages->SetArrayField(TEXT("warnings"), MvvmActionsTextsToJson(Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Warning)));
        Messages->SetArrayField(TEXT("info"), MvvmActionsTextsToJson(Event->GetCompilationMessages(UMVVMBlueprintViewEvent::EMessageType::Info)));
        Obj->SetObjectField(TEXT("messages"), Messages);
        return Obj;
    }

    TSharedPtr<FJsonObject> MvvmActionsConditionToJson(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const UMVVMBlueprintViewCondition* Condition, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("name"), Condition->GetName());
        Obj->SetBoolField(TEXT("enabled"), Condition->bEnabled);
        Obj->SetBoolField(TEXT("compile"), Condition->bCompile);
        Obj->SetObjectField(TEXT("condition"), MvvmActionsPathToJson(WBP, View, Condition->GetConditionPath()));
        Obj->SetObjectField(TEXT("destination"), MvvmActionsPathToJson(WBP, View, Condition->GetDestinationPath()));
        Obj->SetStringField(TEXT("operation"), MvvmActionsEnumToString(Condition->GetOperation()));
        Obj->SetNumberField(TEXT("value"), Condition->GetOperationValue());
        Obj->SetNumberField(TEXT("max_value"), Condition->GetOperationMaxValue());
        Obj->SetArrayField(TEXT("pins"), MvvmActionsPinsToJson(WBP, View, Condition->GetPins()));

        TSharedPtr<FJsonObject> Messages = MakeShared<FJsonObject>();
        Messages->SetArrayField(TEXT("errors"), MvvmActionsTextsToJson(Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Error)));
        Messages->SetArrayField(TEXT("warnings"), MvvmActionsTextsToJson(Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Warning)));
        Messages->SetArrayField(TEXT("info"), MvvmActionsTextsToJson(Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Info)));
        Obj->SetObjectField(TEXT("messages"), Messages);
        return Obj;
    }

    // ------------------------------------------------------------------
    // Parsing / resolution
    // ------------------------------------------------------------------

    FString MvvmActionsListFields(const UStruct* Struct)
    {
        TArray<FString> Names;
        if (Struct)
        {
            for (TFieldIterator<FProperty> It(Struct); It && Names.Num() < 40; ++It)
            {
                Names.Add(It->GetName());
            }
            if (const UClass* Class = Cast<UClass>(Struct))
            {
                for (TFieldIterator<UFunction> It(Class); It && Names.Num() < 60; ++It)
                {
                    if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
                    {
                        Names.Add(It->GetName() + TEXT("()"));
                    }
                }
            }
        }
        return FString::Join(Names, TEXT(", "));
    }

    UStruct* MvvmActionsNextStruct(const FProperty* Property)
    {
        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            return ObjectProperty->PropertyClass;
        }
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            return StructProperty->Struct;
        }
        return nullptr;
    }

    /**
     * Resolve "<kind>:<owner>.<field>..." into an FMVVMBlueprintPropertyPath.
     *   kind = viewmodel | vm | widget | self. Without a prefix the first
     *   segment is matched against viewmodel names, then widget names (the
     *   blueprint's own name means self), else the whole string is a self path.
     * Each field is a property or a function, resolved on the class/struct
     * reached by the previous segment.
     */
    bool MvvmActionsResolvePath(const UWidgetBlueprint* WBP, const UMVVMBlueprintView* View, const FString& InPath,
        FMVVMBlueprintPropertyPath& OutPath, FString& OutError)
    {
        OutPath = FMVVMBlueprintPropertyPath();
        FString Spec = InPath.TrimStartAndEnd();
        FString Kind;
        {
            FString Left, Right;
            if (Spec.Split(TEXT(":"), &Left, &Right))
            {
                Left = Left.TrimStartAndEnd().ToLower();
                if (Left == TEXT("viewmodel") || Left == TEXT("vm") || Left == TEXT("widget") || Left == TEXT("self"))
                {
                    Kind = (Left == TEXT("vm")) ? FString(TEXT("viewmodel")) : Left;
                    Spec = Right.TrimStartAndEnd();
                }
            }
        }

        TArray<FString> Segments;
        Spec.ParseIntoArray(Segments, TEXT("."), /*bCullEmpty*/ true);
        if (Segments.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Empty path '%s'. Use 'viewmodel:<Name>.<Field>', 'widget:<Name>.<Property>' or 'self:<Field>'."), *InPath);
            return false;
        }

        const FMVVMBlueprintViewModelContext* ViewModel = nullptr;
        const UWidget* Widget = nullptr;
        auto FindViewModel = [&](const FString& Name) -> const FMVVMBlueprintViewModelContext*
        {
            return View ? View->FindViewModel(FName(*Name)) : nullptr;
        };
        auto FindWidget = [&](const FString& Name) -> const UWidget*
        {
            return WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*Name)) : nullptr;
        };

        if (Kind.IsEmpty())
        {
            if (Segments.Num() >= 2 && FindViewModel(Segments[0]))
            {
                Kind = TEXT("viewmodel");
            }
            else if (Segments.Num() >= 2 && (Segments[0] == WBP->GetName() || FindWidget(Segments[0])))
            {
                Kind = TEXT("widget");
            }
            else
            {
                Kind = TEXT("self");
            }
        }

        int32 FirstField = 0;
        const UStruct* Current = nullptr;
        if (Kind == TEXT("viewmodel"))
        {
            ViewModel = FindViewModel(Segments[0]);
            if (!ViewModel)
            {
                TArray<FString> Names;
                if (View)
                {
                    for (const FMVVMBlueprintViewModelContext& Ctx : View->GetViewModels())
                    {
                        Names.Add(Ctx.GetViewModelName().ToString());
                    }
                }
                OutError = FString::Printf(TEXT("Viewmodel '%s' not found in path '%s'. Viewmodels on this widget: [%s]. Add one with ui.mvvm_add_viewmodel."),
                    *Segments[0], *InPath, *FString::Join(Names, TEXT(", ")));
                return false;
            }
            Current = ViewModel->GetViewModelClass();
            OutPath.SetViewModelId(ViewModel->GetViewModelId());
            FirstField = 1;
        }
        else if (Kind == TEXT("widget"))
        {
            if (Segments[0] == WBP->GetName())
            {
                Current = MvvmActionsSelfContext(WBP);
                OutPath.SetSelfContext();
            }
            else
            {
                Widget = FindWidget(Segments[0]);
                if (!Widget)
                {
                    OutError = FString::Printf(TEXT("Widget '%s' not found in the widget tree (path '%s'). Use ui.get_widget_tree to list widget names."),
                        *Segments[0], *InPath);
                    return false;
                }
                Current = Widget->GetClass();
                OutPath.SetWidgetName(Widget->GetFName());
            }
            FirstField = 1;
        }
        else
        {
            Current = MvvmActionsSelfContext(WBP);
            OutPath.SetSelfContext();
            FirstField = 0;
        }

        if (FirstField >= Segments.Num())
        {
            OutError = FString::Printf(TEXT("Path '%s' names a source but no field. Append '.<Field>'."), *InPath);
            return false;
        }

        for (int32 Index = FirstField; Index < Segments.Num(); ++Index)
        {
            const FString& Segment = Segments[Index];
            if (!Current)
            {
                OutError = FString::Printf(TEXT("Cannot resolve '%s' in path '%s': the previous segment is not an object or struct."), *Segment, *InPath);
                return false;
            }

            const FName FieldName(*Segment);
            if (const FProperty* Property = FindFProperty<FProperty>(Current, FieldName))
            {
                OutPath.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(Property));
                Current = MvvmActionsNextStruct(Property);
                continue;
            }

            const UClass* CurrentClass = Cast<UClass>(Current);
            if (const UFunction* Function = CurrentClass ? CurrentClass->FindFunctionByName(FieldName) : nullptr)
            {
                OutPath.AppendPropertyPath(WBP, UE::MVVM::FMVVMConstFieldVariant(Function));
                Current = MvvmActionsNextStruct(Function->GetReturnProperty());
                continue;
            }

            OutError = FString::Printf(TEXT("Field '%s' not found on '%s' (path '%s'). If it is a Blueprint variable, compile that Blueprint first. Available: %s"),
                *Segment, *Current->GetName(), *InPath, *MvvmActionsListFields(Current));
            return false;
        }

        if (!OutPath.HasPaths())
        {
            OutError = FString::Printf(TEXT("Path '%s' resolved to no fields."), *InPath);
            return false;
        }
        return true;
    }

    UClass* MvvmActionsLoadClass(const FString& InPath)
    {
        const FString Path = InPath.TrimStartAndEnd();
        if (Path.IsEmpty())
        {
            return nullptr;
        }

        if (Path.StartsWith(TEXT("/")))
        {
            if (UClass* Class = FindObject<UClass>(nullptr, *Path))
            {
                return Class;
            }
            if (UClass* Class = LoadObject<UClass>(nullptr, *Path, {}, LOAD_NoWarn | LOAD_Quiet))
            {
                return Class;
            }

            // Blueprint asset path: "/Game/X/BP" or "/Game/X/BP.BP".
            FString ObjectPath = Path;
            if (!ObjectPath.Contains(TEXT(".")))
            {
                ObjectPath = ObjectPath + TEXT(".") + FPackageName::GetShortName(ObjectPath);
            }
            if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath, {}, LOAD_NoWarn | LOAD_Quiet))
            {
                return Blueprint->GeneratedClass;
            }
            if (UClass* Class = LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")), {}, LOAD_NoWarn | LOAD_Quiet))
            {
                return Class;
            }
            return nullptr;
        }

        // Short native class name ("MyViewModel" or "UMyViewModel").
        if (UClass* Class = FindFirstObject<UClass>(*Path, EFindFirstObjectOptions::NativeFirst))
        {
            return Class;
        }
        if (Path.StartsWith(TEXT("U")))
        {
            return FindFirstObject<UClass>(*Path.RightChop(1), EFindFirstObjectOptions::NativeFirst);
        }
        return nullptr;
    }

    /**
     * Resolve a conversion function:
     *   "/Script/Engine.KismetTextLibrary:Conv_IntToText" (or with '.' before the function),
     *   "KismetTextLibrary.Conv_IntToText", "self:MyFunction" or a bare "MyFunction" on the widget.
     */
    const UFunction* MvvmActionsResolveFunction(const UWidgetBlueprint* WBP, const FString& InSpec, FString& OutError)
    {
        FString Spec = InSpec.TrimStartAndEnd();
        const UClass* OwnerClass = nullptr;
        FString FunctionName;

        if (Spec.StartsWith(TEXT("self:"), ESearchCase::IgnoreCase))
        {
            OwnerClass = MvvmActionsSelfContext(WBP);
            FunctionName = Spec.RightChop(5);
        }
        else if (Spec.StartsWith(TEXT("/")))
        {
            FString Path = Spec;
            if (!Path.Contains(TEXT(":")))
            {
                int32 LastDot = INDEX_NONE;
                if (Path.FindLastChar(TEXT('.'), LastDot))
                {
                    Path[LastDot] = TEXT(':');
                }
            }
            if (const UFunction* Found = FindObject<UFunction>(nullptr, *Path))
            {
                return Found;
            }
            FString ClassPath;
            if (!Path.Split(TEXT(":"), &ClassPath, &FunctionName))
            {
                OutError = FString::Printf(TEXT("Conversion function '%s' must look like '/Script/Module.Class:Function'."), *InSpec);
                return nullptr;
            }
            OwnerClass = MvvmActionsLoadClass(ClassPath);
        }
        else if (Spec.Contains(TEXT(".")))
        {
            FString ClassName;
            Spec.Split(TEXT("."), &ClassName, &FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            OwnerClass = MvvmActionsLoadClass(ClassName);
        }
        else
        {
            OwnerClass = MvvmActionsSelfContext(WBP);
            FunctionName = Spec;
        }

        if (!OwnerClass)
        {
            OutError = FString::Printf(TEXT("Could not resolve the class of conversion function '%s'."), *InSpec);
            return nullptr;
        }
        const UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName));
        if (!Function)
        {
            OutError = FString::Printf(TEXT("Function '%s' not found on '%s'."), *FunctionName, *OwnerClass->GetName());
        }
        return Function;
    }

    UBlueprint* MvvmActionsLoadBlueprint(const FString& InPath)
    {
        FString Path = InPath.TrimStartAndEnd();
        if (!Path.StartsWith(TEXT("/")))
        {
            Path = TEXT("/Game/") + Path;
        }
        if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path, {}, LOAD_NoWarn | LOAD_Quiet))
        {
            return Blueprint;
        }
        if (!Path.Contains(TEXT(".")))
        {
            return LoadObject<UBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), {}, LOAD_NoWarn | LOAD_Quiet);
        }
        return nullptr;
    }

    FMVVMBlueprintViewBinding* MvvmActionsFindBinding(UMVVMBlueprintView* View, const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString IdString;
        if (Params->TryGetStringField(TEXT("binding_id"), IdString) && !IdString.IsEmpty())
        {
            FGuid Id;
            if (!FGuid::Parse(IdString, Id))
            {
                OutError = FString::Printf(TEXT("binding_id '%s' is not a GUID."), *IdString);
                return nullptr;
            }
            if (FMVVMBlueprintViewBinding* Binding = View->GetBinding(Id))
            {
                return Binding;
            }
            OutError = FString::Printf(TEXT("No binding with id %s. Use ui.mvvm_get_view to list bindings."), *IdString);
            return nullptr;
        }

        double IndexValue = -1.0;
        if (Params->TryGetNumberField(TEXT("binding_index"), IndexValue))
        {
            const int32 Index = static_cast<int32>(IndexValue);
            if (FMVVMBlueprintViewBinding* Binding = View->GetBindingAt(Index))
            {
                return Binding;
            }
            OutError = FString::Printf(TEXT("binding_index %d is out of range (%d bindings)."), Index, View->GetNumBindings());
            return nullptr;
        }

        OutError = TEXT("Pass binding_id (GUID from ui.mvvm_get_view) or binding_index.");
        return nullptr;
    }

    UMVVMBlueprintViewEvent* MvvmActionsFindEvent(UMVVMBlueprintView* View, const TSharedPtr<FJsonObject>& Params, int32& OutIndex, FString& OutError)
    {
        TArrayView<TObjectPtr<UMVVMBlueprintViewEvent>> Events = View->GetEvents();

        FString Name;
        if (Params->TryGetStringField(TEXT("event_name"), Name) && !Name.IsEmpty())
        {
            for (int32 Index = 0; Index < Events.Num(); ++Index)
            {
                if (Events[Index] && Events[Index]->GetName() == Name)
                {
                    OutIndex = Index;
                    return Events[Index];
                }
            }
            OutError = FString::Printf(TEXT("No MVVM event named '%s'. Use ui.mvvm_get_view to list events."), *Name);
            return nullptr;
        }

        double IndexValue = -1.0;
        if (Params->TryGetNumberField(TEXT("event_index"), IndexValue))
        {
            const int32 Index = static_cast<int32>(IndexValue);
            if (Events.IsValidIndex(Index) && Events[Index])
            {
                OutIndex = Index;
                return Events[Index];
            }
            OutError = FString::Printf(TEXT("event_index %d is out of range (%d events)."), Index, Events.Num());
            return nullptr;
        }

        OutError = TEXT("Pass event_name (from ui.mvvm_get_view) or event_index.");
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Handlers
    // ------------------------------------------------------------------

    FMonolithActionResult MvvmActionsHandleGetView(const TSharedPtr<FJsonObject>& Params)
    {
        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString WidgetFilter = MonolithUIInternal::GetOptionalString(Params, TEXT("widget"));

        FMonolithActionResult Err;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        const UMVVMBlueprintView* View = Subsystem->GetView(WBP);

        if (!WidgetFilter.IsEmpty()
            && !(WBP->WidgetTree && WBP->WidgetTree->FindWidget(FName(*WidgetFilter))))
        {
            TArray<FString> WidgetNames;
            if (WBP->WidgetTree)
            {
                WBP->WidgetTree->ForEachWidget([&WidgetNames](UWidget* Widget)
                {
                    if (Widget)
                    {
                        WidgetNames.Add(Widget->GetName());
                    }
                });
            }
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("Widget '%s' not found in the widget tree of '%s'. Valid widget names: [%s]."),
                *WidgetFilter, *WBP->GetName(), *FString::Join(WidgetNames, TEXT(", "))));
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetBoolField(TEXT("has_view"), View != nullptr);

        TArray<TSharedPtr<FJsonValue>> ViewModels;
        TArray<TSharedPtr<FJsonValue>> Bindings;
        TArray<TSharedPtr<FJsonValue>> Events;
        TArray<TSharedPtr<FJsonValue>> Conditions;

        if (View)
        {
            for (const FMVVMBlueprintViewModelContext& Ctx : View->GetViewModels())
            {
                ViewModels.Add(MakeShared<FJsonValueObject>(MvvmActionsViewModelToJson(Ctx)));
            }

            const FName FilterName = WidgetFilter.IsEmpty() ? NAME_None : FName(*WidgetFilter);
            const TArrayView<const FMVVMBlueprintViewBinding> AllBindings = View->GetBindings();
            for (int32 Index = 0; Index < AllBindings.Num(); ++Index)
            {
                const FMVVMBlueprintViewBinding& Binding = AllBindings[Index];
                if (!FilterName.IsNone()
                    && Binding.SourcePath.GetWidgetName() != FilterName
                    && Binding.DestinationPath.GetWidgetName() != FilterName)
                {
                    continue;
                }
                Bindings.Add(MakeShared<FJsonValueObject>(MvvmActionsBindingToJson(WBP, View, Binding, Index)));
            }

            const TArrayView<const TObjectPtr<UMVVMBlueprintViewEvent>> AllEvents = View->GetEvents();
            for (int32 Index = 0; Index < AllEvents.Num(); ++Index)
            {
                const UMVVMBlueprintViewEvent* Event = AllEvents[Index];
                if (!Event)
                {
                    continue;
                }
                if (!FilterName.IsNone()
                    && Event->GetEventPath().GetWidgetName() != FilterName
                    && Event->GetDestinationPath().GetWidgetName() != FilterName)
                {
                    continue;
                }
                Events.Add(MakeShared<FJsonValueObject>(MvvmActionsEventToJson(WBP, View, Event, Index)));
            }

            const TArrayView<const TObjectPtr<UMVVMBlueprintViewCondition>> AllConditions = View->GetConditions();
            for (int32 Index = 0; Index < AllConditions.Num(); ++Index)
            {
                const UMVVMBlueprintViewCondition* Condition = AllConditions[Index];
                if (!Condition)
                {
                    continue;
                }
                if (!FilterName.IsNone() && Condition->GetDestinationPath().GetWidgetName() != FilterName)
                {
                    continue;
                }
                Conditions.Add(MakeShared<FJsonValueObject>(MvvmActionsConditionToJson(WBP, View, Condition, Index)));
            }

            if (const UMVVMBlueprintViewSettings* Settings = const_cast<UMVVMBlueprintView*>(View)->GetSettings())
            {
                TSharedPtr<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
                SettingsObj->SetBoolField(TEXT("initialize_sources_on_construct"), Settings->bInitializeSourcesOnConstruct);
                SettingsObj->SetBoolField(TEXT("initialize_bindings_on_construct"), Settings->bInitializeBindingsOnConstruct);
                SettingsObj->SetBoolField(TEXT("initialize_events_on_construct"), Settings->bInitializeEventsOnConstruct);
                SettingsObj->SetBoolField(TEXT("create_view_without_bindings"), Settings->bCreateViewWithoutBindings);
                Result->SetObjectField(TEXT("settings"), SettingsObj);
            }
        }

        Result->SetArrayField(TEXT("viewmodels"), ViewModels);
        Result->SetArrayField(TEXT("bindings"), Bindings);
        Result->SetArrayField(TEXT("events"), Events);
        Result->SetArrayField(TEXT("conditions"), Conditions);
        Result->SetNumberField(TEXT("viewmodel_count"), ViewModels.Num());
        Result->SetNumberField(TEXT("binding_count"), Bindings.Num());
        Result->SetNumberField(TEXT("event_count"), Events.Num());
        Result->SetNumberField(TEXT("condition_count"), Conditions.Num());
        // Totals ignore the widget filter; without a filter they equal the *_count fields.
        Result->SetNumberField(TEXT("total_binding_count"), View ? View->GetBindings().Num() : 0);
        Result->SetNumberField(TEXT("total_event_count"), View ? View->GetEvents().Num() : 0);
        Result->SetNumberField(TEXT("total_condition_count"), View ? View->GetConditions().Num() : 0);
        if (!WidgetFilter.IsEmpty())
        {
            Result->SetStringField(TEXT("widget_filter"), WidgetFilter);
        }
        Result->SetStringField(TEXT("note"),
            TEXT("Binding/event messages are filled by the last Widget Blueprint compile. Paths use 'viewmodel:<Name>.<Field>', 'widget:<Name>.<Field>' or 'self:<Field>'."));
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleAddViewModel(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString ClassPath = Params->GetStringField(TEXT("viewmodel_class"));
        const FString RequestedName = MonolithUIInternal::GetOptionalString(Params, TEXT("name"));
        const FString CreationTypeString = MonolithUIInternal::GetOptionalString(Params, TEXT("creation_type"));
        const FString GlobalIdentifier = MonolithUIInternal::GetOptionalString(Params, TEXT("global_identifier"));
        const FString PropertyPath = MonolithUIInternal::GetOptionalString(Params, TEXT("property_path"));
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        UClass* ViewModelClass = MvvmActionsLoadClass(ClassPath);
        if (!ViewModelClass)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("Viewmodel class '%s' not found. Pass a Blueprint asset path (/Game/.../BP_MyViewModel), a class path (/Script/Module.ClassName) or a native class name."),
                *ClassPath));
        }
        if (!ViewModelClass->ImplementsInterface(UNotifyFieldValueChanged::StaticClass()))
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("Class '%s' does not implement INotifyFieldValueChanged. Viewmodels must derive from MVVMViewModelBase (or implement the interface)."),
                *ViewModelClass->GetPathName()));
        }

        const TArray<EMVVMBlueprintViewModelContextCreationType> Allowed = UE::MVVM::GetAllowedContextCreationType(ViewModelClass);
        TArray<FString> AllowedNames;
        for (EMVVMBlueprintViewModelContextCreationType Type : Allowed)
        {
            AllowedNames.Add(MvvmActionsEnumToString(Type));
        }

        bool bHasCreationType = false;
        EMVVMBlueprintViewModelContextCreationType CreationType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
        if (!CreationTypeString.IsEmpty())
        {
            if (!MvvmActionsEnumFromString(CreationTypeString, CreationType))
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown creation_type '%s'. Valid: %s"),
                    *CreationTypeString, *FString::Join(MvvmActionsEnumNames<EMVVMBlueprintViewModelContextCreationType>(), TEXT(", "))));
            }
            if (!Allowed.Contains(CreationType))
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("creation_type '%s' is not allowed for %s. Allowed: %s"),
                    *CreationTypeString, *ViewModelClass->GetName(), *FString::Join(AllowedNames, TEXT(", "))));
            }
            if (CreationType == EMVVMBlueprintViewModelContextCreationType::Resolver)
            {
                return FMonolithActionResult::Error(TEXT("creation_type 'Resolver' needs a resolver instance, which this action does not author. Add the viewmodel with another creation type and pick the resolver in the Viewmodels panel."));
            }
            if (CreationType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection && GlobalIdentifier.IsEmpty())
            {
                return FMonolithActionResult::Error(TEXT("creation_type 'GlobalViewModelCollection' requires global_identifier."));
            }
            if (CreationType == EMVVMBlueprintViewModelContextCreationType::PropertyPath && PropertyPath.IsEmpty())
            {
                return FMonolithActionResult::Error(TEXT("creation_type 'PropertyPath' requires property_path."));
            }
            bHasCreationType = true;
        }
        else if (Allowed.Contains(EMVVMBlueprintViewModelContextCreationType::CreateInstance))
        {
            // Default to CreateInstance when the class allows it. Otherwise keep the
            // engine default (the first allowed type, set by the context constructor).
            CreationType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
            bHasCreationType = true;
        }

        if (!RequestedName.IsEmpty())
        {
            if (const UMVVMBlueprintView* ExistingView = Subsystem->GetView(WBP))
            {
                if (ExistingView->FindViewModel(FName(*RequestedName)))
                {
                    return FMonolithActionResult::Error(FString::Printf(TEXT("A viewmodel named '%s' already exists on this widget."), *RequestedName));
                }
            }
            FText NameError;
            if (!Subsystem->VerifyViewModelRename(WBP, NAME_None, FName(*RequestedName), NameError))
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid viewmodel name '%s': %s"), *RequestedName, *NameError.ToString()));
            }
        }

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "AddViewModel", "Monolith: Add Viewmodel"));
        WBP->Modify();
        UMVVMBlueprintView* View = Subsystem->RequestView(WBP);
        if (!View)
        {
            Transaction.Cancel();
            return FMonolithActionResult::Error(TEXT("Could not create the MVVM view extension on this Widget Blueprint."));
        }
        View->Modify();

        const FGuid Id = Subsystem->AddViewModel(WBP, ViewModelClass);
        if (!Id.IsValid())
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("UMVVMEditorSubsystem::AddViewModel rejected class '%s'."), *ViewModelClass->GetPathName()));
        }

        if (!RequestedName.IsEmpty())
        {
            const FMVVMBlueprintViewModelContext* Added = View->FindViewModel(Id);
            const FName OldName = Added ? Added->GetViewModelName() : NAME_None;
            // The viewmodel was just added, so nothing references it yet; rename on the
            // view directly (UMVVMEditorSubsystem::RenameViewModel can open a modal
            // dialog about unloaded referencers, which would block the MCP call).
            if (OldName != FName(*RequestedName) && !View->RenameViewModel(OldName, FName(*RequestedName)))
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("Viewmodel was added as '%s' but renaming it to '%s' failed."),
                    *OldName.ToString(), *RequestedName));
            }
        }

        FMVVMBlueprintViewModelContext* Ctx = View->FindViewModel(Id);
        if (!Ctx)
        {
            return FMonolithActionResult::Error(TEXT("Viewmodel was added but could not be found again on the view."));
        }

        if (bHasCreationType)
        {
            Ctx->CreationType = CreationType;
            if (CreationType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection)
            {
                Ctx->GlobalViewModelIdentifier = FName(*GlobalIdentifier);
            }
            if (CreationType == EMVVMBlueprintViewModelContextCreationType::PropertyPath)
            {
                Ctx->ViewModelPropertyPath = PropertyPath;
            }
        }
        if (Ctx->CreationType == EMVVMBlueprintViewModelContextCreationType::Manual)
        {
            // Same defaults the engine applies to Manual contexts on load.
            Ctx->bOptional = true;
            Ctx->bCreateSetterFunction = true;
        }
        // A freshly constructed context has the private upgrade flag cleared, so the next
        // load of the view (asset reload or transaction undo/redo) runs the old-asset
        // upgrade in FMVVMBlueprintViewModelContext::PostSerialize and overwrites
        // bOptional/bCreateSetterFunction. Mark the context as already upgraded so the
        // values written here survive.
        const bool bUpgradeFlagSet = MvvmActionsMarkContextUpgraded(View, *Ctx);

        View->OnViewModelsUpdated.Broadcast();
        MvvmActionsFinish(WBP, /*bStructural*/ true, bCompile);

        // Read the stored context back after the compile so the response shows the final state.
        const FMVVMBlueprintViewModelContext* Stored = View->FindViewModel(Id);
        if (!Stored)
        {
            return FMonolithActionResult::Error(TEXT("Viewmodel was added but is missing from the view after compiling."));
        }
        TSharedPtr<FJsonObject> ViewModelJson = MvvmActionsViewModelToJson(*Stored);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetObjectField(TEXT("viewmodel"), ViewModelJson);
        Result->SetStringField(TEXT("creation_type_source"), CreationTypeString.IsEmpty() ? TEXT("default") : TEXT("explicit"));
        if (!bUpgradeFlagSet)
        {
            Result->SetStringField(TEXT("warning"),
                TEXT("Could not mark the viewmodel context as upgraded; optional/create_setter may change after the asset is reloaded or an undo."));
        }
        Result->SetArrayField(TEXT("allowed_creation_types"), MvvmActionsStringsToJson(AllowedNames));
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleRemoveViewModel(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString Name = Params->GetStringField(TEXT("name"));
        const bool bRemoveBindings = MonolithUIInternal::GetOptionalBool(Params, TEXT("remove_bindings"), false);
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        UMVVMBlueprintView* View = Subsystem->GetView(WBP);
        const FMVVMBlueprintViewModelContext* Ctx = View ? View->FindViewModel(FName(*Name)) : nullptr;
        if (!Ctx)
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Viewmodel '%s' not found. Use ui.mvvm_get_view to list viewmodels."), *Name));
        }
        if (!Ctx->bCanRemove)
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Viewmodel '%s' is marked as not removable."), *Name));
        }
        const FGuid Id = Ctx->GetViewModelId();

        auto PathUses = [&Id](const FMVVMBlueprintPropertyPath& Path)
        {
            return Path.GetViewModelId() == Id;
        };
        // A conversion function or event argument can carry the viewmodel in a pin
        // while the binding's own source path is empty.
        auto PinsUse = [&PathUses](TArrayView<const FMVVMBlueprintPin> Pins)
        {
            for (const FMVVMBlueprintPin& Pin : Pins)
            {
                if (Pin.UsedPathAsValue() && PathUses(Pin.GetPath()))
                {
                    return true;
                }
            }
            return false;
        };

        TArray<int32> BindingIndices;
        const TArrayView<const FMVVMBlueprintViewBinding> Bindings = static_cast<const UMVVMBlueprintView*>(View)->GetBindings();
        for (int32 Index = 0; Index < Bindings.Num(); ++Index)
        {
            const FMVVMBlueprintViewBinding& Binding = Bindings[Index];
            const UMVVMBlueprintViewConversionFunction* Forward = Binding.Conversion.GetConversionFunction(true);
            const UMVVMBlueprintViewConversionFunction* Backward = Binding.Conversion.GetConversionFunction(false);
            if (PathUses(Binding.SourcePath) || PathUses(Binding.DestinationPath)
                || (Forward && PinsUse(Forward->GetPins()))
                || (Backward && PinsUse(Backward->GetPins())))
            {
                BindingIndices.Add(Index);
            }
        }
        TArray<UMVVMBlueprintViewEvent*> DependentEvents;
        for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : View->GetEvents())
        {
            if (Event && (PathUses(Event->GetEventPath()) || PathUses(Event->GetDestinationPath()) || PinsUse(Event->GetPins())))
            {
                DependentEvents.Add(Event);
            }
        }

        if ((BindingIndices.Num() > 0 || DependentEvents.Num() > 0) && !bRemoveBindings)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("Viewmodel '%s' is used by %d binding(s) and %d event(s). Pass remove_bindings=true to remove them with the viewmodel."),
                *Name, BindingIndices.Num(), DependentEvents.Num()));
        }

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "RemoveViewModel", "Monolith: Remove Viewmodel"));
        WBP->Modify();
        View->Modify();
        for (int32 Index = BindingIndices.Num() - 1; Index >= 0; --Index)
        {
            View->RemoveBindingAt(BindingIndices[Index]);
        }
        for (UMVVMBlueprintViewEvent* Event : DependentEvents)
        {
            View->RemoveEvent(Event);
        }
        const bool bRemoved = View->RemoveViewModel(Id);
        if (!bRemoved)
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("UMVVMBlueprintView::RemoveViewModel failed for '%s'."), *Name));
        }
        MvvmActionsFinish(WBP, /*bStructural*/ true, bCompile);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetStringField(TEXT("removed"), Name);
        Result->SetNumberField(TEXT("removed_bindings"), BindingIndices.Num());
        Result->SetNumberField(TEXT("removed_events"), DependentEvents.Num());
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleAddBinding(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString SourceString = Params->GetStringField(TEXT("source"));
        const FString DestinationString = Params->GetStringField(TEXT("destination"));
        const FString ModeString = MonolithUIInternal::GetOptionalString(Params, TEXT("mode"), TEXT("OneWayToDestination"));
        const FString ExecutionModeString = MonolithUIInternal::GetOptionalString(Params, TEXT("execution_mode"));
        const FString ConversionString = MonolithUIInternal::GetOptionalString(Params, TEXT("conversion_function"));
        const FString ConversionArgument = MonolithUIInternal::GetOptionalString(Params, TEXT("conversion_argument"));
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        EMVVMBindingMode Mode = EMVVMBindingMode::OneWayToDestination;
        if (!MvvmActionsEnumFromString(ModeString, Mode))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown mode '%s'. Valid: %s"),
                *ModeString, *FString::Join(MvvmActionsEnumNames<EMVVMBindingMode>(), TEXT(", "))));
        }

        bool bHasExecutionMode = false;
        EMVVMExecutionMode ExecutionMode = EMVVMExecutionMode::Immediate;
        if (!ExecutionModeString.IsEmpty())
        {
            if (!MvvmActionsEnumFromString(ExecutionModeString, ExecutionMode))
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown execution_mode '%s'. Valid: %s"),
                    *ExecutionModeString, *FString::Join(MvvmActionsEnumNames<EMVVMExecutionMode>(), TEXT(", "))));
            }
            bHasExecutionMode = true;
        }

        // Resolve everything before touching the asset so a bad parameter leaves it untouched.
        const UMVVMBlueprintView* ExistingView = Subsystem->GetView(WBP);
        FString PathError;
        FMVVMBlueprintPropertyPath SourcePath;
        if (!MvvmActionsResolvePath(WBP, ExistingView, SourceString, SourcePath, PathError))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("source: %s"), *PathError));
        }
        FMVVMBlueprintPropertyPath DestinationPath;
        if (!MvvmActionsResolvePath(WBP, ExistingView, DestinationString, DestinationPath, PathError))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("destination: %s"), *PathError));
        }

        const UFunction* ConversionFunction = nullptr;
        if (!ConversionString.IsEmpty())
        {
            if (!UE::MVVM::IsForwardBinding(Mode))
            {
                return FMonolithActionResult::Error(TEXT("conversion_function is only supported for modes that write to the destination (OneTimeToDestination, OneWayToDestination, TwoWay)."));
            }
            FString FunctionError;
            ConversionFunction = MvvmActionsResolveFunction(WBP, ConversionString, FunctionError);
            if (!ConversionFunction)
            {
                return FMonolithActionResult::Error(FString::Printf(TEXT("conversion_function: %s"), *FunctionError));
            }
            if (!Subsystem->IsValidConversionFunction(WBP, ConversionFunction, SourcePath, DestinationPath))
            {
                return FMonolithActionResult::Error(FString::Printf(
                    TEXT("'%s' is not a valid conversion function from %s to %s (it must be pure, return a value assignable to the destination and take an argument compatible with the source)."),
                    *ConversionFunction->GetName(), *SourceString, *DestinationString));
            }
        }

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "AddBinding", "Monolith: Add MVVM Binding"));
        WBP->Modify();
        UMVVMBlueprintView* View = Subsystem->RequestView(WBP);
        if (!View)
        {
            Transaction.Cancel();
            return FMonolithActionResult::Error(TEXT("Could not create the MVVM view extension on this Widget Blueprint."));
        }
        View->Modify();

        const FGuid BindingId = Subsystem->AddBinding(WBP).BindingId;
        // Re-fetch by id before every call: the bindings array may reallocate.
        auto GetBinding = [View, BindingId]() -> FMVVMBlueprintViewBinding*
        {
            return View->GetBinding(BindingId);
        };

        if (FMVVMBlueprintViewBinding* Binding = GetBinding())
        {
            Subsystem->SetBindingTypeForBinding(WBP, *Binding, Mode);
        }
        if (FMVVMBlueprintViewBinding* Binding = GetBinding())
        {
            Subsystem->SetSourcePathForBinding(WBP, *Binding, SourcePath);
        }
        if (FMVVMBlueprintViewBinding* Binding = GetBinding())
        {
            Subsystem->SetDestinationPathForBinding(WBP, *Binding, DestinationPath, /*bAllowEventConversion*/ false);
        }

        TArray<FString> Warnings;
        if (ConversionFunction)
        {
            if (FMVVMBlueprintViewBinding* Binding = GetBinding())
            {
                Subsystem->SetSourceToDestinationConversionFunction(WBP, *Binding, FMVVMBlueprintFunctionReference(WBP, ConversionFunction));
            }

            FMVVMBlueprintViewBinding* Binding = GetBinding();
            UMVVMBlueprintViewConversionFunction* Conversion = Binding ? Binding->Conversion.GetConversionFunction(true) : nullptr;
            if (!Conversion)
            {
                if (Binding)
                {
                    Subsystem->RemoveBinding(WBP, *Binding);
                }
                return FMonolithActionResult::Error(FString::Printf(TEXT("The MVVM editor rejected conversion function '%s'; the binding was not added."), *ConversionFunction->GetName()));
            }

            // Setting a conversion function clears the binding's source path; the
            // source now feeds one of the conversion function's arguments.
            const TArrayView<const FMVVMBlueprintPin> Pins = Conversion->GetPins();
            const FMVVMBlueprintPin* Target = nullptr;
            TArray<FString> PinNames;
            for (const FMVVMBlueprintPin& Pin : Pins)
            {
                const TArrayView<const FName> Names = Pin.GetId().GetNames();
                const FString PinName = Names.Num() > 0 ? Names.Last().ToString() : FString();
                PinNames.Add(PinName);
                if (!Target && (ConversionArgument.IsEmpty() ? !Pin.UsedPathAsValue() : PinName.Equals(ConversionArgument, ESearchCase::IgnoreCase)))
                {
                    Target = &Pin;
                }
            }
            if (!Target)
            {
                Subsystem->RemoveBinding(WBP, *Binding);
                return FMonolithActionResult::Error(FString::Printf(TEXT("Conversion function '%s' has no argument %s. Arguments: [%s]. The binding was not added."),
                    *ConversionFunction->GetName(),
                    ConversionArgument.IsEmpty() ? TEXT("free for the source") : *FString::Printf(TEXT("'%s'"), *ConversionArgument),
                    *FString::Join(PinNames, TEXT(", "))));
            }
            const FMVVMBlueprintPinId TargetId = Target->GetId();
            Subsystem->SetPathForConversionFunctionArgument(WBP, *Binding, TargetId, SourcePath, /*bSourceToDestination*/ true);
        }

        if (bHasExecutionMode)
        {
            if (FMVVMBlueprintViewBinding* Binding = GetBinding())
            {
                Subsystem->OverrideExecutionModeForBinding(WBP, *Binding, ExecutionMode);
            }
        }

        MvvmActionsFinish(WBP, /*bStructural*/ false, bCompile);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        const TArrayView<const FMVVMBlueprintViewBinding> Bindings = static_cast<const UMVVMBlueprintView*>(View)->GetBindings();
        for (int32 Index = 0; Index < Bindings.Num(); ++Index)
        {
            if (Bindings[Index].BindingId == BindingId)
            {
                Result->SetObjectField(TEXT("binding"), MvvmActionsBindingToJson(WBP, View, Bindings[Index], Index));
                break;
            }
        }
        Result->SetBoolField(TEXT("compiled"), bCompile);
        if (Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), MvvmActionsStringsToJson(Warnings));
        }
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleRemoveBinding(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        UMVVMBlueprintView* View = Subsystem->GetView(WBP);
        if (!View)
        {
            return FMonolithActionResult::Error(TEXT("This Widget Blueprint has no MVVM view (no viewmodels or bindings)."));
        }

        FString FindError;
        FMVVMBlueprintViewBinding* Binding = MvvmActionsFindBinding(View, Params, FindError);
        if (!Binding)
        {
            return FMonolithActionResult::Error(FindError);
        }
        const FString RemovedId = MvvmActionsGuidToString(Binding->BindingId);
        const FString RemovedName = Binding->GetDisplayNameString(WBP);

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "RemoveBinding", "Monolith: Remove MVVM Binding"));
        WBP->Modify();
        Subsystem->RemoveBinding(WBP, *Binding);
        MvvmActionsFinish(WBP, /*bStructural*/ false, bCompile);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetStringField(TEXT("removed_id"), RemovedId);
        Result->SetStringField(TEXT("removed_display_name"), RemovedName);
        Result->SetNumberField(TEXT("remaining_bindings"), View->GetNumBindings());
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleAddEvent(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString EventString = Params->GetStringField(TEXT("event"));
        const FString DestinationString = Params->GetStringField(TEXT("destination"));
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        // `arguments` may arrive as an object or as a JSON string (MCP transport quirk).
        TSharedPtr<FJsonObject> Arguments;
        {
            const TSharedPtr<FJsonObject>* ArgumentsObject = nullptr;
            FString ArgumentsString;
            if (Params->TryGetObjectField(TEXT("arguments"), ArgumentsObject) && ArgumentsObject)
            {
                Arguments = *ArgumentsObject;
            }
            else if (Params->TryGetStringField(TEXT("arguments"), ArgumentsString) && !ArgumentsString.IsEmpty())
            {
                Arguments = FMonolithJsonUtils::Parse(ArgumentsString);
                if (!Arguments.IsValid())
                {
                    return FMonolithActionResult::Error(TEXT("arguments must be an object mapping argument name to a source path."));
                }
            }
        }

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        const UMVVMBlueprintView* ExistingView = Subsystem->GetView(WBP);
        FString PathError;
        FMVVMBlueprintPropertyPath EventPath;
        // Events are delegates on widgets; default the kind to widget when no prefix is given.
        const FString EventSpec = EventString.Contains(TEXT(":")) ? EventString : FString(TEXT("widget:")) + EventString;
        if (!MvvmActionsResolvePath(WBP, ExistingView, EventSpec, EventPath, PathError))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("event: %s"), *PathError));
        }
        if (!UMVVMBlueprintViewEvent::Supports(WBP, EventPath))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("event '%s' is not a multicast delegate (e.g. widget:Button_0.OnClicked). Use ui.list_widget_events."), *EventString));
        }
        FMVVMBlueprintPropertyPath DestinationPath;
        if (!MvvmActionsResolvePath(WBP, ExistingView, DestinationString, DestinationPath, PathError))
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("destination: %s"), *PathError));
        }

        TMap<FName, FMVVMBlueprintPropertyPath> ArgumentPaths;
        if (Arguments.IsValid())
        {
            // Key type differs between engines (FString on 5.7, FSharedString on 5.8).
            for (const auto& Pair : Arguments->Values)
            {
                const FString Key = MonolithKeyToString(Pair.Key);
                FString ValuePath;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(ValuePath))
                {
                    return FMonolithActionResult::Error(FString::Printf(TEXT("arguments.%s must be a path string."), *Key));
                }
                FMVVMBlueprintPropertyPath ArgumentPath;
                if (!MvvmActionsResolvePath(WBP, ExistingView, ValuePath, ArgumentPath, PathError))
                {
                    return FMonolithActionResult::Error(FString::Printf(TEXT("arguments.%s: %s"), *Key, *PathError));
                }
                ArgumentPaths.Add(FName(*Key), ArgumentPath);
            }
        }

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "AddEvent", "Monolith: Add MVVM Event"));
        WBP->Modify();
        UMVVMBlueprintView* View = Subsystem->RequestView(WBP);
        if (!View)
        {
            Transaction.Cancel();
            return FMonolithActionResult::Error(TEXT("Could not create the MVVM view extension on this Widget Blueprint."));
        }
        View->Modify();

        UMVVMBlueprintViewEvent* Event = Subsystem->AddEvent(WBP);
        if (!Event)
        {
            return FMonolithActionResult::Error(TEXT("MVVM events are disabled. Enable 'Allow Binding Event' in Project Settings > Plugins > UMG Model View Viewmodel."));
        }
        Subsystem->SetEventPath(Event, EventPath, /*bRequestBindingConversion*/ false);
        Subsystem->SetEventDestinationPath(Event, DestinationPath);

        TArray<FString> Warnings;
        for (const TPair<FName, FMVVMBlueprintPropertyPath>& Pair : ArgumentPaths)
        {
            const FMVVMBlueprintPin* Found = nullptr;
            TArray<FString> PinNames;
            for (const FMVVMBlueprintPin& Pin : Event->GetPins())
            {
                const TArrayView<const FName> Names = Pin.GetId().GetNames();
                if (Names.Num() > 0)
                {
                    PinNames.Add(Names.Last().ToString());
                    if (!Found && Names.Last() == Pair.Key)
                    {
                        Found = &Pin;
                    }
                }
            }
            if (!Found)
            {
                Warnings.Add(FString::Printf(TEXT("Argument '%s' not found on the destination. Arguments: [%s]"),
                    *Pair.Key.ToString(), *FString::Join(PinNames, TEXT(", "))));
                continue;
            }
            const FMVVMBlueprintPinId PinId = Found->GetId();
            Subsystem->SetEventArgumentPath(Event, PinId, Pair.Value);
        }

        MvvmActionsFinish(WBP, /*bStructural*/ false, bCompile);

        int32 EventIndex = INDEX_NONE;
        const TArrayView<TObjectPtr<UMVVMBlueprintViewEvent>> Events = View->GetEvents();
        for (int32 Index = 0; Index < Events.Num(); ++Index)
        {
            if (Events[Index] == Event)
            {
                EventIndex = Index;
                break;
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetObjectField(TEXT("event"), MvvmActionsEventToJson(WBP, View, Event, EventIndex));
        Result->SetBoolField(TEXT("compiled"), bCompile);
        if (Warnings.Num() > 0)
        {
            Result->SetArrayField(TEXT("warnings"), MvvmActionsStringsToJson(Warnings));
        }
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleRemoveEvent(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
        if (!WBP) return Err;
        UMVVMEditorSubsystem* Subsystem = MvvmActionsGetSubsystem(Err);
        if (!Subsystem) return Err;

        UMVVMBlueprintView* View = Subsystem->GetView(WBP);
        if (!View)
        {
            return FMonolithActionResult::Error(TEXT("This Widget Blueprint has no MVVM view (no viewmodels or bindings)."));
        }

        FString FindError;
        int32 Index = INDEX_NONE;
        UMVVMBlueprintViewEvent* Event = MvvmActionsFindEvent(View, Params, Index, FindError);
        if (!Event)
        {
            return FMonolithActionResult::Error(FindError);
        }
        const FString RemovedName = Event->GetName();

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "RemoveEvent", "Monolith: Remove MVVM Event"));
        WBP->Modify();
        Subsystem->RemoveEvent(WBP, Event);
        MvvmActionsFinish(WBP, /*bStructural*/ false, bCompile);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
        Result->SetStringField(TEXT("removed"), RemovedName);
        Result->SetNumberField(TEXT("removed_index"), Index);
        Result->SetNumberField(TEXT("remaining_events"), View->GetEvents().Num());
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    FMonolithActionResult MvvmActionsHandleSetFieldNotify(const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithActionResult Err;
        if (MvvmActionsRefuseDuringPIE(Err)) return Err;

        const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
        const FString VariableString = Params->GetStringField(TEXT("variable"));
        const bool bEnabled = MonolithUIInternal::GetOptionalBool(Params, TEXT("enabled"), true);
        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);

        UBlueprint* Blueprint = MvvmActionsLoadBlueprint(AssetPath);
        if (!Blueprint)
        {
            return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
        }
        const UClass* ParentClass = Blueprint->ParentClass;
        if (!ParentClass || !ParentClass->ImplementsInterface(UNotifyFieldValueChanged::StaticClass()))
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("'%s' does not implement INotifyFieldValueChanged, so its variables cannot be FieldNotify. Parent it to MVVMViewModelBase (or a UserWidget)."),
                *Blueprint->GetName()));
        }

        const FName VariableName(*VariableString);
        const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
        if (VariableIndex == INDEX_NONE)
        {
            TArray<FString> Names;
            for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
            {
                Names.Add(Variable.VarName.ToString());
            }
            return FMonolithActionResult::Error(FString::Printf(TEXT("Member variable '%s' not found on %s. Variables: [%s]"),
                *VariableString, *Blueprint->GetName(), *FString::Join(Names, TEXT(", "))));
        }

        FScopedTransaction Transaction(NSLOCTEXT("MonolithUIMVVM", "SetFieldNotify", "Monolith: Set FieldNotify"));
        Blueprint->Modify();
        if (bEnabled)
        {
            FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VariableName, nullptr, FBlueprintMetadata::MD_FieldNotify, TEXT(""));
        }
        else
        {
            FBlueprintEditorUtils::RemoveFieldNotifyFromAllMetadata(Blueprint, VariableName);
            FBlueprintEditorUtils::RemoveBlueprintVariableMetaData(Blueprint, VariableName, nullptr, FBlueprintMetadata::MD_FieldNotify);
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }

        const int32 IndexAfter = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName);
        const bool bIsFieldNotify = IndexAfter != INDEX_NONE && Blueprint->NewVariables[IndexAfter].HasMetaData(FBlueprintMetadata::MD_FieldNotify);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        Result->SetStringField(TEXT("variable"), VariableString);
        Result->SetBoolField(TEXT("field_notify"), bIsFieldNotify);
        Result->SetBoolField(TEXT("compiled"), bCompile);
        return FMonolithActionResult::Success(Result);
    }

    const TCHAR* MvvmActionsPathHelp()
    {
        return TEXT("Path grammar: 'viewmodel:<ViewmodelName>.<Field>[.<Field>...]', 'widget:<WidgetName>.<Property>' or 'self:<Field>'. "
                    "Fields may be properties or functions. Without a prefix the first segment is matched against viewmodel names, then widget names.");
    }
}

namespace MonolithUI
{
    void FMVVMActions::Register(FMonolithToolRegistry& Registry)
    {
        const FString Category = TEXT("MVVM");

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_get_view"),
            TEXT("Read the UMG MVVM view of a Widget Blueprint: viewmodels (class, creation type, flags), bindings (id, mode, source/destination paths, ")
            TEXT("conversion functions with their argument pins, enabled/compile flags, compiler messages), events (widget delegate -> function, ")
            TEXT("argument pins), conditions and view settings. Read-only. Paths are returned in the grammar the mvvm_ write actions accept."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleGetView),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Optional(TEXT("widget"), TEXT("string"), TEXT("Only return bindings/events/conditions whose source or destination is this widget name. *_count fields are filtered; total_*_count fields are not. Unknown names return an error listing valid widget names"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_add_viewmodel"),
            TEXT("Add a viewmodel to a Widget Blueprint through UMVVMEditorSubsystem. The class must implement INotifyFieldValueChanged ")
            TEXT("(e.g. a Blueprint or C++ subclass of MVVMViewModelBase). Optional name and creation type (Manual, CreateInstance, ")
            TEXT("GlobalViewModelCollection, PropertyPath; Context on UE 5.8). Resolver is not authored by this action."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleAddViewModel),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Required(TEXT("viewmodel_class"), TEXT("string"), TEXT("Viewmodel class: Blueprint asset path (/Game/.../BP_MyViewModel), class path (/Script/Module.Class) or native class name"))
                .Optional(TEXT("name"), TEXT("string"), TEXT("Viewmodel name (default: derived from the class name)"))
                .Optional(TEXT("creation_type"), TEXT("string"), TEXT("Manual | CreateInstance | GlobalViewModelCollection | PropertyPath (default: CreateInstance if the class allows it, otherwise the first allowed type). Manual sets optional and create_setter"))
                .Optional(TEXT("global_identifier"), TEXT("string"), TEXT("Required for creation_type=GlobalViewModelCollection"))
                .Optional(TEXT("property_path"), TEXT("string"), TEXT("Required for creation_type=PropertyPath"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_remove_viewmodel"),
            TEXT("Remove a viewmodel from a Widget Blueprint. Refuses when bindings or events still use it unless remove_bindings=true."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleRemoveViewModel),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Required(TEXT("name"), TEXT("string"), TEXT("Viewmodel name (see mvvm_get_view)"))
                .Optional(TEXT("remove_bindings"), TEXT("boolean"), TEXT("Also remove bindings and events that reference the viewmodel"), TEXT("false"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_add_binding"),
            FString(TEXT("Add an MVVM binding (source path -> destination path) through UMVVMEditorSubsystem, with binding mode, optional execution mode and ")
                    TEXT("optional source-to-destination conversion function whose argument receives the source. ")) + MvvmActionsPathHelp(),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleAddBinding),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Required(TEXT("source"), TEXT("string"), TEXT("Source path, e.g. viewmodel:VM_Player.Health"))
                .Required(TEXT("destination"), TEXT("string"), TEXT("Destination path, e.g. widget:HealthBar.Percent or self:SetHealthText"))
                .Optional(TEXT("mode"), TEXT("string"), TEXT("OneTimeToDestination | OneWayToDestination | TwoWay | OneWayToSource"), TEXT("OneWayToDestination"))
                .Optional(TEXT("execution_mode"), TEXT("string"), TEXT("Override: Immediate | Delayed | Tick | DelayedWhenSharedElseImmediate"))
                .Optional(TEXT("conversion_function"), TEXT("string"), TEXT("Conversion function, e.g. /Script/Engine.KismetTextLibrary:Conv_IntToText, KismetTextLibrary.Conv_IntToText or self:MyFunction"))
                .Optional(TEXT("conversion_argument"), TEXT("string"), TEXT("Conversion function argument that receives the source (default: first argument)"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards (fills binding messages)"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_remove_binding"),
            TEXT("Remove an MVVM binding by id (from mvvm_get_view) or index."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleRemoveBinding),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Optional(TEXT("binding_id"), TEXT("string"), TEXT("Binding GUID from mvvm_get_view"))
                .Optional(TEXT("binding_index"), TEXT("integer"), TEXT("Binding index from mvvm_get_view (used when binding_id is absent)"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_add_event"),
            FString(TEXT("Add an MVVM event: when a widget multicast delegate fires (e.g. widget:Button_0.OnClicked), call a function on a viewmodel ")
                    TEXT("or the widget. Optional arguments map destination argument names to source paths. Requires 'Allow Binding Event' in the MVVM project settings. "))
                + MvvmActionsPathHelp(),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleAddEvent),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Required(TEXT("event"), TEXT("string"), TEXT("Delegate path; widget kind assumed without a prefix, e.g. Button_0.OnClicked"))
                .Required(TEXT("destination"), TEXT("string"), TEXT("Function to call, e.g. viewmodel:VM_Player.ResetScore or self:OnResetClicked"))
                .Optional(TEXT("arguments"), TEXT("object"), TEXT("Map of destination argument name -> source path, e.g. {\"NewValue\": \"widget:Slider_0.Value\"}"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_remove_event"),
            TEXT("Remove an MVVM event by name (from mvvm_get_view) or index."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleRemoveEvent),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
                .Optional(TEXT("event_name"), TEXT("string"), TEXT("Event object name from mvvm_get_view"))
                .Optional(TEXT("event_index"), TEXT("integer"), TEXT("Event index from mvvm_get_view (used when event_name is absent)"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);

        Registry.RegisterAction(
            TEXT("ui"), TEXT("mvvm_set_field_notify"),
            TEXT("Turn the FieldNotify flag of a Blueprint member variable on or off, so MVVM bindings are notified when it changes. ")
            TEXT("The Blueprint must implement INotifyFieldValueChanged (MVVMViewModelBase subclasses and UserWidgets do)."),
            FMonolithActionHandler::CreateStatic(&MvvmActionsHandleSetFieldNotify),
            FParamSchemaBuilder()
                .RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path (e.g. a viewmodel Blueprint)"))
                .Required(TEXT("variable"), TEXT("string"), TEXT("Member variable name"))
                .Optional(TEXT("enabled"), TEXT("boolean"), TEXT("true to make the variable FieldNotify, false to clear it"), TEXT("true"))
                .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Blueprint afterwards"), TEXT("true"))
                .Build(),
            Category);
    }
}

#else // WITH_MVVM

namespace MonolithUI
{
    void FMVVMActions::Register(FMonolithToolRegistry& /*Registry*/)
    {
        // ModelViewViewModel plugin not enabled for this build (or MONOLITH_RELEASE_BUILD=1):
        // no mvvm_ actions are registered.
    }
}

#endif // WITH_MVVM
