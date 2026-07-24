// Copyright tumourlove. All Rights Reserved.
#include "Actions/Hoisted/GradientActions.h"

// Monolith registry
#include "MonolithToolRegistry.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Core / packaging
#include "UObject/Package.h"                    // UPackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/UObjectGlobals.h"             // LoadObject, NewObject, CreatePackage
#include "Misc/PackageName.h"                   // FPackageName::LongPackageNameToFilename / GetAssetPackageExtension

// Materials -- runtime + editor-only write API
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Materials/MaterialParameters.h"
#endif

// MaterialEditor module -- required for UpdateMaterialInstance after static switch writes
#include "MaterialEditingLibrary.h"

// Asset registry + asset tools (unique naming)
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"

// Math
#include "Math/Color.h"

// MonolithUI shared color parser (no-degamma path; matches MD_UI material expectations).
#include "MonolithUICommon.h"

namespace MonolithUI::GradientInternal
{
    /** Hard ceiling on stop count. */
    static constexpr int32 GMaxStops = 8;

    /** Check whether Parent has a scalar parameter with the given name. */
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

    /** Check whether Parent has a vector parameter with the given name. */
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

    /** Check whether Parent has a static-switch parameter with the given name. */
    static bool ParentHasStaticSwitchParam(UMaterialInterface* Parent, const FName& ParamName)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;
        Parent->GetAllStaticSwitchParameterInfo(Infos, Guids);
        for (const FMaterialParameterInfo& Info : Infos)
        {
            if (Info.Name == ParamName)
            {
                return true;
            }
        }
        return false;
    }

    /** Single parsed stop. */
    struct FGradientStop
    {
        float Pos = 0.0f;
        FLinearColor Color = FLinearColor::Black;
    };
} // namespace MonolithUI::GradientInternal

FMonolithActionResult MonolithUI::FGradientActions::HandleCreateGradientMidFromSpec(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::GradientInternal;

    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
    }

    FString ParentPath;
    if (!Params->TryGetStringField(TEXT("parent_material"), ParentPath) || ParentPath.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: parent_material"), -32602);
    }
    FString Destination;
    if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: destination"), -32602);
    }
    if (!Destination.StartsWith(TEXT("/")))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("destination must be a long package path like /Game/Foo/Bar (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT(".uasset")))
    {
        Destination = Destination.LeftChop(7);
    }

    const TSharedPtr<FJsonObject>* SpecObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("spec"), SpecObjPtr) || !SpecObjPtr || !SpecObjPtr->IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing or invalid required param: spec (object)"), -32602);
    }
    const TSharedPtr<FJsonObject>& SpecObj = *SpecObjPtr;

    const TArray<TSharedPtr<FJsonValue>>* StopsArr = nullptr;
    if (!SpecObj->TryGetArrayField(TEXT("stops"), StopsArr) || !StopsArr || StopsArr->Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("spec.stops must be a non-empty array"), -32602);
    }

    TArray<FString> Warnings;
    int32 RequestedStopCount = StopsArr->Num();
    int32 StopsToApply = FMath::Min(RequestedStopCount, GMaxStops);
    if (RequestedStopCount > GMaxStops)
    {
        Warnings.Add(FString::Printf(
            TEXT("spec.stops contained %d entries; clamping to %d (parent material supports at most %d)"),
            RequestedStopCount, GMaxStops, GMaxStops));
    }

    TArray<FGradientStop> Stops;
    Stops.Reserve(StopsToApply);
    for (int32 i = 0; i < StopsToApply; ++i)
    {
        const TSharedPtr<FJsonValue>& Entry = (*StopsArr)[i];
        const TSharedPtr<FJsonObject>* StopObjPtr = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(StopObjPtr) || !StopObjPtr || !StopObjPtr->IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("spec.stops[%d] must be an object with {pos, color}"), i),
                -32602);
        }
        const TSharedPtr<FJsonObject>& StopObj = *StopObjPtr;

        double PosD = 0.0;
        if (!StopObj->TryGetNumberField(TEXT("pos"), PosD))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("spec.stops[%d].pos must be a number in [0,1]"), i),
                -32602);
        }
        const float Pos = FMath::Clamp((float)PosD, 0.0f, 1.0f);

        FString ColorStr;
        if (!StopObj->TryGetStringField(TEXT("color"), ColorStr) || ColorStr.IsEmpty())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("spec.stops[%d].color must be a non-empty hex or 'R,G,B[,A]' string"), i),
                -32602);
        }
        FLinearColor Parsed;
        if (!MonolithUI::TryParseColor(ColorStr, Parsed))
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Cannot parse spec.stops[%d].color '%s' (expected #RGB/#RRGGBB/#RRGGBBAA or 'R,G,B[,A]')"),
                    i, *ColorStr),
                -32602);
        }

        FGradientStop Stop;
        Stop.Pos = Pos;
        Stop.Color = Parsed;
        Stops.Add(Stop);
    }

    double AngleDegD = 0.0;
    const bool bHasAngle = SpecObj->TryGetNumberField(TEXT("angle_deg"), AngleDegD);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
    if (!Parent)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("parent_material '%s' not found or not a UMaterialInterface"), *ParentPath),
            -32602);
    }

    // Validate parent BEFORE creating any asset.
    if (!ParentHasScalarParam(Parent, FName(TEXT("Stop0Pos")))
        || !ParentHasVectorParam(Parent, FName(TEXT("Stop0Color"))))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("parent material '%s' does not expose Stop0Pos/Stop0Color parameters required for gradient MID factory"),
                *ParentPath),
            -32602);
    }
    // Soft-check additional stops -- truncate with warning if parent runs out.
    for (int32 i = 1; i < StopsToApply; ++i)
    {
        const FName PosName(*FString::Printf(TEXT("Stop%dPos"), i));
        const FName ColorName(*FString::Printf(TEXT("Stop%dColor"), i));
        if (!ParentHasScalarParam(Parent, PosName) || !ParentHasVectorParam(Parent, ColorName))
        {
            Warnings.Add(FString::Printf(
                TEXT("parent material '%s' is missing %s or %s -- truncating stops from %d to %d"),
                *ParentPath, *PosName.ToString(), *ColorName.ToString(), StopsToApply, i));
            StopsToApply = i;
            break;
        }
    }

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    FString UniquePackageName;
    FString UniqueAssetName;
    AssetToolsModule.Get().CreateUniqueAssetName(
        Destination, /*Suffix=*/FString(),
        /*out*/ UniquePackageName, /*out*/ UniqueAssetName);

    UPackage* Package = CreatePackage(*UniquePackageName);
    if (!Package)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create package '%s'"), *UniquePackageName),
            -32603);
    }
    Package->FullyLoad();

    UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(
        Package, FName(*UniqueAssetName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (!MIC)
    {
        return FMonolithActionResult::Error(TEXT("Failed to create UMaterialInstanceConstant"), -32603);
    }

    // SetParentEditorOnly is the documented entry point in UE 5.7 -- requires
    // a follow-up PostEditChange (called below).
    MIC->SetParentEditorOnly(Parent);

    bool bAngleApplied = false;
    if (bHasAngle && ParentHasScalarParam(Parent, FName(TEXT("Angle"))))
    {
        MIC->SetScalarParameterValueEditorOnly(
            FMaterialParameterInfo(FName(TEXT("Angle"))), (float)AngleDegD);
        bAngleApplied = true;
    }
    else if (bHasAngle)
    {
        Warnings.Add(FString::Printf(
            TEXT("spec.angle_deg was provided but parent material '%s' has no 'Angle' scalar parameter -- ignored"),
            *ParentPath));
    }

    for (int32 i = 0; i < StopsToApply; ++i)
    {
        const FName PosName(*FString::Printf(TEXT("Stop%dPos"), i));
        const FName ColorName(*FString::Printf(TEXT("Stop%dColor"), i));
        MIC->SetScalarParameterValueEditorOnly(
            FMaterialParameterInfo(PosName), Stops[i].Pos);
        MIC->SetVectorParameterValueEditorOnly(
            FMaterialParameterInfo(ColorName), Stops[i].Color);
    }

    // Apply UseStopN switches explicitly so we don't inherit ambiguous parent defaults.
    int32 SwitchesApplied = 0;
    for (int32 i = 0; i < GMaxStops; ++i)
    {
        const FName SwitchName(*FString::Printf(TEXT("UseStop%d"), i));
        if (!ParentHasStaticSwitchParam(Parent, SwitchName))
        {
            continue;
        }
        const bool bEnable = (i < StopsToApply);
        MIC->SetStaticSwitchParameterValueEditorOnly(
            FMaterialParameterInfo(SwitchName), bEnable);
        ++SwitchesApplied;
    }

    // Optional: CornerRadii + WidgetSize for material-level SDF rounding.
    const TArray<TSharedPtr<FJsonValue>>* CornerArr = nullptr;
    if (SpecObj->TryGetArrayField(TEXT("corner_radii"), CornerArr) && CornerArr && CornerArr->Num() >= 4)
    {
        MIC->SetVectorParameterValueEditorOnly(
            FMaterialParameterInfo(FName(TEXT("CornerRadii"))),
            FLinearColor(
                (*CornerArr)[0]->AsNumber(),
                (*CornerArr)[1]->AsNumber(),
                (*CornerArr)[2]->AsNumber(),
                (*CornerArr)[3]->AsNumber()));
    }
    const TArray<TSharedPtr<FJsonValue>>* SizeArr = nullptr;
    if (SpecObj->TryGetArrayField(TEXT("widget_size"), SizeArr) && SizeArr && SizeArr->Num() >= 2)
    {
        MIC->SetVectorParameterValueEditorOnly(
            FMaterialParameterInfo(FName(TEXT("WidgetSize"))),
            FLinearColor(
                (*SizeArr)[0]->AsNumber(),
                (*SizeArr)[1]->AsNumber(), 0.0f, 0.0f));
    }

    MIC->PostEditChange();

    // UpdateMaterialInstance is the canonical post-static-switch refresh in UE 5.7.
    if (SwitchesApplied > 0)
    {
        UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
    }

    FAssetRegistryModule::AssetCreated(MIC);
    Package->MarkPackageDirty();

    if (bSave)
    {
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
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), UniquePackageName);
    Result->SetStringField(TEXT("parent_material"), Parent->GetPathName());
    Result->SetNumberField(TEXT("stops_applied"), (double)StopsToApply);
    Result->SetNumberField(TEXT("switches_applied"), (double)SwitchesApplied);
    Result->SetBoolField(TEXT("angle_applied"), bAngleApplied);
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

void MonolithUI::FGradientActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("create_gradient_mid_from_spec"),
        TEXT("Parameter-driven gradient MID factory. Creates a UMaterialInstanceConstant from a caller-supplied parent material "
             "(any material exposing StopNPos scalar + StopNColor vector + UseStopN static-switch params, optionally 'Angle' scalar). "
             "Supports 1-8 stops. "
             "Params: parent_material (string, required, long package path), "
             "destination (string, required, /Game/... output MID path without .uasset), "
             "spec (object, required: { angle_deg?: number, stops: [{ pos: number in [0,1], color: '#RRGGBB' | '#RRGGBBAA' | 'R,G,B[,A]' }, ...] }), "
             "save (bool, optional, default true). "
             "Validates parent has Stop0Pos/Stop0Color before creating any asset -- returns -32602 on incompatible parent."),
        FMonolithActionHandler::CreateStatic(&MonolithUI::FGradientActions::HandleCreateGradientMidFromSpec));
}
