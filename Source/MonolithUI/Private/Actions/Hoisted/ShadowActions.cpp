// Copyright tumourlove. All Rights Reserved.
#include "Actions/Hoisted/ShadowActions.h"

// Monolith registry
#include "MonolithToolRegistry.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Core / loading
#include "UObject/UObjectGlobals.h"             // LoadObject, NewObject
#include "UObject/Package.h"                    // UPackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "Misc/PackageName.h"                   // FPackageName::LongPackageNameToFilename

// UMG runtime + editor
#include "Blueprint/WidgetTree.h"               // UWidgetTree::FindWidget / ConstructWidget
#include "Components/Widget.h"                  // UWidget::GetParent
#include "Components/PanelWidget.h"             // UPanelWidget::GetChildIndex / InsertChildAt
#include "Components/Image.h"                   // UImage::SetBrush
#include "Components/CanvasPanelSlot.h"         // UCanvasPanelSlot setters
#include "WidgetBlueprint.h"                    // UWidgetBlueprint

// Slate brush (FSlateBrush + ESlateBrushDrawType)
#include "Styling/SlateBrush.h"

// Kismet editor
#include "Kismet2/BlueprintEditorUtils.h"       // MarkBlueprintAsStructurallyModified
#include "Kismet2/KismetEditorUtilities.h"      // CompileBlueprint

// Materials
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"

// Asset registry + asset tools (unique naming for saved-MID case)
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

// Math
#include "Math/Color.h"
#include "Math/Vector2D.h"

// MonolithUI shared color parser.
#include "MonolithUICommon.h"

namespace MonolithUI::ShadowInternal
{
    /** Hard cap on shadow layers (CSS allows N, we render 2). */
    static constexpr int32 GMaxShadowLayers = 2;

    /** Does the parent material expose a scalar parameter with the given name? */
    static bool ParentHasScalarParam(UMaterialInterface* Parent, const FName& ParamName)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;
        Parent->GetAllScalarParameterInfo(Infos, Guids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            if (Info.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }

    /** Does the parent material expose a vector parameter with the given name? */
    static bool ParentHasVectorParam(UMaterialInterface* Parent, const FName& ParamName)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;
        Parent->GetAllVectorParameterInfo(Infos, Guids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            if (Info.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }

    /** Parsed shadow spec (one layer). */
    struct FShadowSpec
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Blur = 0.0f;
        float Spread = 0.0f;
        FLinearColor Color = FLinearColor::Black;
        bool bInset = false;
    };

    /** Parse a single shadow object from JSON. Returns false with OutError populated on malformed entries. */
    static bool ParseShadowSpec(
        const TSharedPtr<FJsonObject>& Obj,
        const FString& Context,
        FShadowSpec& OutSpec,
        FString& OutError)
    {
        if (!Obj.IsValid())
        {
            OutError = FString::Printf(TEXT("%s must be an object"), *Context);
            return false;
        }

        FString ColorStr;
        if (!Obj->TryGetStringField(TEXT("color"), ColorStr) || ColorStr.IsEmpty())
        {
            OutError = FString::Printf(TEXT("%s.color is required (hex or 'R,G,B[,A]')"), *Context);
            return false;
        }
        if (!MonolithUI::TryParseColor(ColorStr, OutSpec.Color))
        {
            OutError = FString::Printf(
                TEXT("Cannot parse %s.color '%s' (expected #RGB/#RRGGBB/#RRGGBBAA or 'R,G,B[,A]')"),
                *Context, *ColorStr);
            return false;
        }

        double D = 0.0;
        if (Obj->TryGetNumberField(TEXT("x"), D))      { OutSpec.X      = (float)D; }
        if (Obj->TryGetNumberField(TEXT("y"), D))      { OutSpec.Y      = (float)D; }
        if (Obj->TryGetNumberField(TEXT("blur"), D))   { OutSpec.Blur   = FMath::Max(0.0f, (float)D); }
        if (Obj->TryGetNumberField(TEXT("spread"), D)) { OutSpec.Spread = (float)D; }
        Obj->TryGetBoolField(TEXT("inset"), OutSpec.bInset);
        return true;
    }
} // namespace MonolithUI::ShadowInternal

FMonolithActionResult MonolithUI::FShadowActions::HandleApplyBoxShadow(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::ShadowInternal;

    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
    }

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
    }
    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: widget_name"), -32602);
    }
    FString ShadowMaterialPath;
    if (!Params->TryGetStringField(TEXT("shadow_material_path"), ShadowMaterialPath)
        || ShadowMaterialPath.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("Missing or empty required param: shadow_material_path"), -32602);
    }

    const TSharedPtr<FJsonObject>* SingleObjPtr = nullptr;
    const bool bHasSingle = Params->TryGetObjectField(TEXT("shadow"), SingleObjPtr)
        && SingleObjPtr && SingleObjPtr->IsValid();
    const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
    const bool bHasArray = Params->TryGetArrayField(TEXT("shadows"), ArrPtr) && ArrPtr;

    if (bHasSingle && bHasArray)
    {
        return FMonolithActionResult::Error(
            TEXT("Provide either 'shadow' (object) OR 'shadows' (array), not both"), -32602);
    }
    if (!bHasSingle && !bHasArray)
    {
        return FMonolithActionResult::Error(
            TEXT("Either 'shadow' (object) or 'shadows' (array) is required"), -32602);
    }

    TArray<FShadowSpec> Specs;
    TArray<FString> Warnings;

    if (bHasSingle)
    {
        FShadowSpec Spec;
        FString Err;
        if (!ParseShadowSpec(*SingleObjPtr, TEXT("shadow"), Spec, Err))
        {
            return FMonolithActionResult::Error(Err, -32602);
        }
        Specs.Add(Spec);
    }
    else
    {
        if (ArrPtr->Num() == 0)
        {
            return FMonolithActionResult::Error(
                TEXT("'shadows' array must contain at least one entry"), -32602);
        }

        const int32 RequestedLayers = ArrPtr->Num();
        const int32 LayersToApply = FMath::Min(RequestedLayers, GMaxShadowLayers);
        if (RequestedLayers > GMaxShadowLayers)
        {
            Warnings.Add(FString::Printf(
                TEXT("only first %d shadow layers applied (received %d, cap is %d)"),
                GMaxShadowLayers, RequestedLayers, GMaxShadowLayers));
        }

        for (int32 i = 0; i < LayersToApply; ++i)
        {
            const TSharedPtr<FJsonValue>& V = (*ArrPtr)[i];
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!V.IsValid() || !V->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("shadows[%d] must be an object"), i), -32602);
            }
            FShadowSpec Spec;
            FString Err;
            const FString Ctx = FString::Printf(TEXT("shadows[%d]"), i);
            if (!ParseShadowSpec(*ObjPtr, Ctx, Spec, Err))
            {
                return FMonolithActionResult::Error(Err, -32602);
            }
            Specs.Add(Spec);
        }
    }

    // Optional saved-MID destination -- when set, build UMaterialInstanceConstant
    // assets per layer; when omitted, build transient UMaterialInstanceDynamic.
    FString SavedMIDDestination;
    const bool bSaveMID = Params->TryGetStringField(TEXT("shadow_mid_destination"), SavedMIDDestination)
        && !SavedMIDDestination.IsEmpty();
    if (bSaveMID && !SavedMIDDestination.StartsWith(TEXT("/")))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("shadow_mid_destination must be a long package path like /Game/Foo/Bar (got '%s')"),
                *SavedMIDDestination),
            -32602);
    }
    if (bSaveMID && SavedMIDDestination.EndsWith(TEXT(".uasset")))
    {
        SavedMIDDestination = SavedMIDDestination.LeftChop(7);
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    if (!WBP)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget Blueprint not found at asset_path '%s'"), *AssetPath),
            -32602);
    }
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget Blueprint '%s' has no WidgetTree"), *AssetPath),
            -32603);
    }
    UWidget* Target = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Target)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget named '%s' not found in Widget Blueprint '%s'"),
                *WidgetName, *AssetPath),
            -32602);
    }

    // Sibling-insert requires a parent panel. If target IS the root, bail rather
    // than silently restructuring the tree.
    UPanelWidget* Parent = Target->GetParent();
    if (!Parent)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("target widget '%s' has no parent panel; cannot insert sibling shadow"),
                *WidgetName),
            -32603);
    }
    int32 TargetIdx = Parent->GetChildIndex(Target);
    if (TargetIdx == INDEX_NONE)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Target widget '%s' not found in its own parent panel -- tree corrupt?"),
                *WidgetName),
            -32603);
    }

    UMaterialInterface* ShadowParent = LoadObject<UMaterialInterface>(nullptr, *ShadowMaterialPath);
    if (!ShadowParent)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("shadow_material_path '%s' not found or not a UMaterialInterface"),
                *ShadowMaterialPath),
            -32602);
    }
    // Minimum contract: ShadowColor vector + BlurRadius scalar.
    TArray<FString> MissingParams;
    if (!ParentHasVectorParam(ShadowParent, FName(TEXT("ShadowColor"))))
    {
        MissingParams.Add(TEXT("ShadowColor(vector)"));
    }
    if (!ParentHasScalarParam(ShadowParent, FName(TEXT("BlurRadius"))))
    {
        MissingParams.Add(TEXT("BlurRadius(scalar)"));
    }
    if (MissingParams.Num() > 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("shadow material '%s' is missing required params: %s"),
                *ShadowMaterialPath, *FString::Join(MissingParams, TEXT(", "))),
            -32602);
    }

    const bool bHasOffsetParam      = ParentHasVectorParam(ShadowParent, FName(TEXT("Offset")));
    const bool bHasSpreadParam      = ParentHasScalarParam(ShadowParent, FName(TEXT("Spread")));
    const bool bHasInsetParam       = ParentHasScalarParam(ShadowParent, FName(TEXT("Inset")));
    const bool bHasShadowSizeParam  = ParentHasVectorParam(ShadowParent, FName(TEXT("ShadowSize")));

    UCanvasPanelSlot* TargetCanvasSlot = Cast<UCanvasPanelSlot>(Target->Slot);
    const bool bTargetOnCanvas = (TargetCanvasSlot != nullptr);
    FVector2D TargetPos(0.0f, 0.0f);
    FVector2D TargetSize(0.0f, 0.0f);
    FAnchors TargetAnchors;
    FVector2D TargetAlignment(0.0f, 0.0f);

    // Explicit target_size override from caller -- preferred when slot geometry
    // hasn't been applied yet.
    const TArray<TSharedPtr<FJsonValue>>* TargetSizeArr = nullptr;
    if (Params->TryGetArrayField(TEXT("target_size"), TargetSizeArr)
        && TargetSizeArr && TargetSizeArr->Num() >= 2)
    {
        TargetSize.X = (*TargetSizeArr)[0]->AsNumber();
        TargetSize.Y = (*TargetSizeArr)[1]->AsNumber();
    }

    if (bTargetOnCanvas)
    {
        TargetPos = TargetCanvasSlot->GetPosition();
        if (TargetSize.X <= 0.0 || TargetSize.Y <= 0.0)
        {
            TargetSize = TargetCanvasSlot->GetSize();
        }
        if (TargetSize.X <= 0.0 || TargetSize.Y <= 0.0)
        {
            Target->ForceLayoutPrepass();
            const FVector2D Desired = Target->GetDesiredSize();
            if (Desired.X > 0.0 && Desired.Y > 0.0)
            {
                TargetSize = Desired;
            }
        }
        TargetAnchors   = TargetCanvasSlot->GetAnchors();
        TargetAlignment = TargetCanvasSlot->GetAlignment();
    }
    else
    {
        Warnings.Add(FString::Printf(
            TEXT("parent panel of '%s' is not a CanvasPanel (type '%s') -- shadow sibling(s) will be inserted but x/y offset and blur/spread-expansion will NOT be applied to slot geometry"),
            *WidgetName, *Parent->GetClass()->GetName()));
    }

    FAssetToolsModule* AssetToolsModule = bSaveMID
        ? &FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"))
        : nullptr;

    // Insert sequence: walk specs in REVERSE, each time inserting at TargetIdx so
    // the first-listed spec ends up furthest back (CSS semantics).
    TArray<FString> ShadowWidgetNames;
    ShadowWidgetNames.Reserve(Specs.Num());

    for (int32 LayerIndex = Specs.Num() - 1; LayerIndex >= 0; --LayerIndex)
    {
        const FShadowSpec& Spec = Specs[LayerIndex];

        UMaterialInterface* ShadowMID = nullptr;
        if (bSaveMID)
        {
            FString UniquePackageName;
            FString UniqueAssetName;
            const FString PerLayerBase = Specs.Num() > 1
                ? FString::Printf(TEXT("%s_L%d"), *SavedMIDDestination, LayerIndex)
                : SavedMIDDestination;
            AssetToolsModule->Get().CreateUniqueAssetName(
                PerLayerBase, /*Suffix=*/FString(), UniquePackageName, UniqueAssetName);

            UPackage* Package = CreatePackage(*UniquePackageName);
            if (!Package)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("Failed to create package '%s' for saved shadow MID"), *UniquePackageName),
                    -32603);
            }
            Package->FullyLoad();

            UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
                Package, FName(*UniqueAssetName),
                RF_Public | RF_Standalone | RF_Transactional);
            if (!MIC)
            {
                return FMonolithActionResult::Error(
                    TEXT("Failed to create UMaterialInstanceConstant for shadow MID"), -32603);
            }
            MIC->SetParentEditorOnly(ShadowParent);

            MIC->SetVectorParameterValueEditorOnly(
                FMaterialParameterInfo(FName(TEXT("ShadowColor"))), Spec.Color);
            MIC->SetScalarParameterValueEditorOnly(
                FMaterialParameterInfo(FName(TEXT("BlurRadius"))), Spec.Blur);
            if (bHasOffsetParam)
            {
                MIC->SetVectorParameterValueEditorOnly(
                    FMaterialParameterInfo(FName(TEXT("Offset"))),
                    FLinearColor(Spec.X, Spec.Y, 0.0f, 0.0f));
            }
            if (bHasSpreadParam)
            {
                MIC->SetScalarParameterValueEditorOnly(
                    FMaterialParameterInfo(FName(TEXT("Spread"))), Spec.Spread);
            }
            if (bHasInsetParam)
            {
                MIC->SetScalarParameterValueEditorOnly(
                    FMaterialParameterInfo(FName(TEXT("Inset"))), Spec.bInset ? 1.0f : 0.0f);
            }
            if (bHasShadowSizeParam)
            {
                MIC->SetVectorParameterValueEditorOnly(
                    FMaterialParameterInfo(FName(TEXT("ShadowSize"))),
                    FLinearColor(TargetSize.X, TargetSize.Y, 0.0f, 0.0f));
            }
            MIC->PostEditChange();
            FAssetRegistryModule::AssetCreated(MIC);
            Package->MarkPackageDirty();

            const FString PackageFilename = FPackageName::LongPackageNameToFilename(
                Package->GetName(), FPackageName::GetAssetPackageExtension());
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            const bool bSaved = UPackage::SavePackage(Package, MIC, *PackageFilename, SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *PackageFilename),
                    -32603);
            }
            ShadowMID = MIC;
        }
        else
        {
            UMaterialInstanceDynamic* DynMID = UMaterialInstanceDynamic::Create(
                ShadowParent, WBP->WidgetTree);
            if (!DynMID)
            {
                return FMonolithActionResult::Error(
                    TEXT("UMaterialInstanceDynamic::Create returned null"), -32603);
            }
            DynMID->SetVectorParameterValue(FName(TEXT("ShadowColor")), Spec.Color);
            DynMID->SetScalarParameterValue(FName(TEXT("BlurRadius")), Spec.Blur);
            if (bHasOffsetParam)
            {
                DynMID->SetVectorParameterValue(FName(TEXT("Offset")),
                    FLinearColor(Spec.X, Spec.Y, 0.0f, 0.0f));
            }
            if (bHasSpreadParam)
            {
                DynMID->SetScalarParameterValue(FName(TEXT("Spread")), Spec.Spread);
            }
            if (bHasInsetParam)
            {
                DynMID->SetScalarParameterValue(FName(TEXT("Inset")), Spec.bInset ? 1.0f : 0.0f);
            }
            if (bHasShadowSizeParam)
            {
                DynMID->SetVectorParameterValue(FName(TEXT("ShadowSize")),
                    FLinearColor(TargetSize.X, TargetSize.Y, 0.0f, 0.0f));
            }
            ShadowMID = DynMID;
        }

        const FString ShadowName = FString::Printf(TEXT("%s_Shadow%d"), *WidgetName, LayerIndex);
        UImage* ShadowImg = WBP->WidgetTree->ConstructWidget<UImage>(
            UImage::StaticClass(), FName(*ShadowName));
        if (!ShadowImg)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to construct UImage '%s'"), *ShadowName),
                -32603);
        }

        FSlateBrush Brush;
        Brush.DrawAs = ESlateBrushDrawType::Image;
        Brush.TintColor = FSlateColor(FLinearColor::White);
        Brush.SetResourceObject(ShadowMID);
        const float Expansion = Spec.Blur + FMath::Max(0.0f, Spec.Spread);
        if (bTargetOnCanvas)
        {
            Brush.ImageSize = FVector2D(
                FMath::Max(1.0, (double)TargetSize.X + 2.0 * (double)Expansion),
                FMath::Max(1.0, (double)TargetSize.Y + 2.0 * (double)Expansion));
        }
        else
        {
            Brush.ImageSize = FVector2D(64.0, 64.0);
        }
        ShadowImg->SetBrush(Brush);

        UPanelSlot* NewSlot = Parent->InsertChildAt(TargetIdx, ShadowImg);
        if (!NewSlot)
        {
            ShadowImg->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("InsertChildAt returned null inserting shadow '%s' into parent '%s'"),
                    *ShadowName, *Parent->GetClass()->GetName()),
                -32603);
        }

        // Register in WidgetVariableNameToGuidMap so the WBP compiler validation
        // pass (WidgetBlueprintCompiler.cpp:794) finds the entry on re-compile.
        MonolithUI::RegisterCreatedWidget(WBP, ShadowImg);

        if (UCanvasPanelSlot* ShadowCanvasSlot = Cast<UCanvasPanelSlot>(NewSlot))
        {
            if (bTargetOnCanvas)
            {
                ShadowCanvasSlot->SetAnchors(TargetAnchors);
                ShadowCanvasSlot->SetAlignment(TargetAlignment);
                ShadowCanvasSlot->SetPosition(FVector2D(
                    (double)TargetPos.X + (double)Spec.X - (double)Expansion,
                    (double)TargetPos.Y + (double)Spec.Y - (double)Expansion));
                ShadowCanvasSlot->SetSize(FVector2D(
                    FMath::Max(1.0, (double)TargetSize.X + 2.0 * (double)Expansion),
                    FMath::Max(1.0, (double)TargetSize.Y + 2.0 * (double)Expansion)));
                ShadowCanvasSlot->SetZOrder(TargetCanvasSlot->GetZOrder() - 1);
            }
        }

        ShadowWidgetNames.Insert(ShadowName, 0); // preserve original 0..N-1 ordering in report
        ++TargetIdx;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("widget_name"), WidgetName);
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& N : ShadowWidgetNames)
        {
            Arr.Add(MakeShared<FJsonValueString>(N));
        }
        Result->SetArrayField(TEXT("shadow_widgets"), Arr);
    }
    Result->SetNumberField(TEXT("layers_applied"), (double)Specs.Num());
    Result->SetBoolField(TEXT("compiled"), bCompile);
    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& W : Warnings)
        {
            Arr.Add(MakeShared<FJsonValueString>(W));
        }
        Result->SetArrayField(TEXT("warnings"), Arr);
    }
    return FMonolithActionResult::Success(Result);
}

void MonolithUI::FShadowActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("apply_box_shadow"),
        TEXT("Insert sibling UImage widget(s) behind a target widget in an authored WBP, each backed by a transient MID "
             "parented to a caller-supplied shadow material. "
             "Params: asset_path (string, required, WBP long path), widget_name (string, required, target widget name), "
             "shadow_material_path (string, required -- parent must expose at minimum ShadowColor vector + BlurRadius scalar; "
             "optionally Offset vector, Spread scalar, Inset scalar), "
             "shadow (object, single-layer: {x,y,blur,spread,color,inset}) OR shadows (array of same, capped at 2 layers), "
             "shadow_mid_destination (string, optional -- if set, saves a UMaterialInstanceConstant per layer at /Game/... instead of transient MID), "
             "compile (bool, optional, default true). "
             "Fails with -32602 on malformed params or incompatible parent material; -32603 on tree/asset errors."),
        FMonolithActionHandler::CreateStatic(&MonolithUI::FShadowActions::HandleApplyBoxShadow));
}
