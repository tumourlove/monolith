// Category I: Accessibility Bridge — 4 actions (v1 = lint/audit + metadata stamping)
// 4.I.1 enforce_focus_ring
// 4.I.2 wrap_with_reduce_motion_gate
// 4.I.3 set_text_scale_binding
// 4.I.4 apply_high_contrast_variant
//
// NOTE: Full implementations require Blueprint graph editing (adding subsystem checks,
// animation-start branches, font-size bindings). v1 ships as AUDIT + METADATA STAMP actions:
// scan a folder, report issues, optionally stamp a bool UPROPERTY on CDO if the WBP
// exposes one. Full graph-editing fixups are a v2 follow-up.
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonButtonBase.h"
#include "CommonTextBlock.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace MonolithCommonUIAccessibility
{
	static TArray<FAssetData> GetWbpsInFolder(const FString& FolderPath)
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*FolderPath));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> Out;
		ARM.Get().GetAssets(Filter, Out);
		return Out;
	}

	// ----- 4.I.1 enforce_focus_ring --------------------------------------------
	// Audit: scan UCommonButtonBase widgets in a folder of WBPs, report those
	// without a style asset assigned (a heuristic: no style = no focus ring by default).

	static FMonolithActionResult HandleEnforceFocusRing(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("folder_path"), FolderPath))
			return FMonolithActionResult::Error(TEXT("folder_path required"));

		const TArray<FAssetData> Found = GetWbpsInFolder(FolderPath);
		int32 WbpsScanned = 0, ButtonsScanned = 0, Unstyled = 0;
		TArray<TSharedPtr<FJsonValue>> Offenders;

		for (const FAssetData& AD : Found)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp || !Wbp->WidgetTree) continue;
			WbpsScanned++;

			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(W))
				{
					ButtonsScanned++;
					FObjectProperty* StyleProp = FindFProperty<FObjectProperty>(UCommonButtonBase::StaticClass(), TEXT("Style"));
					UObject* StyleVal = StyleProp ? StyleProp->GetObjectPropertyValue_InContainer(Btn) : nullptr;
					if (!StyleVal)
					{
						TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
						O->SetStringField(TEXT("wbp_path"), AD.GetObjectPathString());
						O->SetStringField(TEXT("button_name"), Btn->GetName());
						Offenders.Add(MakeShared<FJsonValueObject>(O));
						Unstyled++;
					}
				}
			});
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		Result->SetNumberField(TEXT("wbps_scanned"), WbpsScanned);
		Result->SetNumberField(TEXT("buttons_scanned"), ButtonsScanned);
		Result->SetNumberField(TEXT("unstyled_button_count"), Unstyled);
		Result->SetArrayField(TEXT("offenders"), Offenders);
		Result->SetStringField(TEXT("recommendation"), TEXT("Assign a UCommonButtonStyle class to each button via apply_style_to_widget. Focus ring thickness ≥3px mandated for hospice-accessibility."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- Shared: stamp a named bool UPROPERTY on a WBP CDO -------------------

	static int32 StampBoolOnWbpCdo(UWidgetBlueprint* Wbp, FName PropName, bool Value)
	{
		if (!Wbp || !Wbp->GeneratedClass) return 0;
		UObject* CDO = Wbp->GeneratedClass->GetDefaultObject();
		if (!CDO) return 0;
		FBoolProperty* P = FindFProperty<FBoolProperty>(Wbp->GeneratedClass, PropName);
		if (!P) return 0;
		P->SetPropertyValue_InContainer(CDO, Value);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();
		return 1;
	}

	// ----- 4.I.2 wrap_with_reduce_motion_gate ----------------------------------
	// Stamp bool UPROPERTY `bRespectReduceMotion` on the WBP CDO. WBP author must check this
	// in their animation-trigger BP logic. Report list of WBPs that don't expose the property.

	static FMonolithActionResult HandleWrapWithReduceMotionGate(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("folder_path"), FolderPath))
			return FMonolithActionResult::Error(TEXT("folder_path required"));

		const TArray<FAssetData> Found = GetWbpsInFolder(FolderPath);
		int32 Scanned = 0, Stamped = 0;
		TArray<TSharedPtr<FJsonValue>> Skipped;
		TArray<TSharedPtr<FJsonValue>> MissingLegacy; // backwards-compat mirror

		// Bug #3 fix (2026-05-16 UI gap audit): when stamp doesn't land we now
		// return a diagnostic-rich skipped[] entry naming the missing UPROPERTY
		// AND suggested parent classes the caller can re-parent to. Previously
		// the action returned stamped=0 with no cause attached, forcing the
		// caller to infer that the parent class lacked the property.
		//
		// Suggested parent classes: any UCommonActivatableWidget subclass that
		// ships a bRespectReduceMotion UPROPERTY. The list is conservative —
		// Monolith doesn't itself ship a base class; project owners typically
		// add the property either to a project-local activatable widget or to
		// Tokenforge's UTokenforgeActivatableWidget. The Tier-2 add_widget_variable
		// action (Phase 2) will let callers stamp the UPROPERTY directly when
		// no satisfying parent exists.
		static const TArray<FString> SuggestedParentClasses = {
			TEXT("UTokenforgeActivatableWidget"),
			TEXT("UMonolithReduceMotionAwareWidget")
		};

		for (const FAssetData& AD : Found)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp) continue;
			Scanned++;

			const int32 Result = StampBoolOnWbpCdo(Wbp, TEXT("bRespectReduceMotion"), true);
			if (Result > 0) { Stamped++; }
			else
			{
				const FString WbpPath = AD.GetObjectPathString();

				// New diagnostic-rich skipped[] entry.
				TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
				SkipObj->SetStringField(TEXT("wbp"), WbpPath);
				SkipObj->SetStringField(TEXT("reason"), TEXT("missing_property"));
				SkipObj->SetStringField(TEXT("missing_property"), TEXT("bRespectReduceMotion"));

				TArray<TSharedPtr<FJsonValue>> SuggestedArr;
				for (const FString& Cls : SuggestedParentClasses)
				{
					SuggestedArr.Add(MakeShared<FJsonValueString>(Cls));
				}
				SkipObj->SetArrayField(TEXT("suggested_parent_classes"), SuggestedArr);
				Skipped.Add(MakeShared<FJsonValueObject>(SkipObj));

				// Legacy missing_property[] mirror, preserved one release for
				// callers that already parse the prior shape.
				TSharedPtr<FJsonObject> Legacy = MakeShared<FJsonObject>();
				Legacy->SetStringField(TEXT("wbp_path"), WbpPath);
				Legacy->SetStringField(TEXT("missing"), TEXT("bRespectReduceMotion bool UPROPERTY"));
				MissingLegacy.Add(MakeShared<FJsonValueObject>(Legacy));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		Result->SetNumberField(TEXT("wbps_scanned"), Scanned);
		Result->SetNumberField(TEXT("wbps_stamped"), Stamped);
		Result->SetArrayField(TEXT("skipped"), Skipped);
		Result->SetArrayField(TEXT("missing_property"), MissingLegacy);
		Result->SetStringField(TEXT("next_step"), TEXT("For WBPs in skipped[]: either re-parent to a class in suggested_parent_classes that exposes bRespectReduceMotion, or use ui::add_widget_variable (Phase 2) to add the UPROPERTY directly. Then gate animation triggers on UAccessibilitySubsystem::IsReduceMotionEnabled()."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 4.I.3 set_text_scale_binding ----------------------------------------
	// Stamp bool UPROPERTY `bHonorAccessibilityTextScale` on UCommonTextBlock widgets' owning WBP.

	static FMonolithActionResult HandleSetTextScaleBinding(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("folder_path"), FolderPath))
			return FMonolithActionResult::Error(TEXT("folder_path required"));

		const TArray<FAssetData> Found = GetWbpsInFolder(FolderPath);
		int32 Scanned = 0, Stamped = 0;
		TArray<TSharedPtr<FJsonValue>> Missing;

		for (const FAssetData& AD : Found)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp) continue;
			Scanned++;

			const int32 Result = StampBoolOnWbpCdo(Wbp, TEXT("bHonorAccessibilityTextScale"), true);
			if (Result > 0) { Stamped++; }
			else
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("wbp_path"), AD.GetObjectPathString());
				Missing.Add(MakeShared<FJsonValueObject>(O));
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		Result->SetNumberField(TEXT("wbps_scanned"), Scanned);
		Result->SetNumberField(TEXT("wbps_stamped"), Stamped);
		Result->SetArrayField(TEXT("missing_property"), Missing);
		Result->SetStringField(TEXT("next_step"), TEXT("Add UPROPERTY bool bHonorAccessibilityTextScale + PreConstruct branch that multiplies font size by UAccessibilitySubsystem::GetTextScale()."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 4.I.4 apply_high_contrast_variant -----------------------------------
	// Forwards to batch_retheme logic — swap one style class for its high-contrast variant.

	static FMonolithActionResult HandleApplyHighContrastVariant(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath, StyleNormal, StyleHC;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("folder_path"), FolderPath) ||
			!Params->TryGetStringField(TEXT("normal_style"), StyleNormal) ||
			!Params->TryGetStringField(TEXT("high_contrast_style"), StyleHC))
			return FMonolithActionResult::Error(TEXT("folder_path, normal_style, high_contrast_style required"));

		UClass* OldClass = LoadClass<UObject>(nullptr, *StyleNormal);
		UClass* NewClass = LoadClass<UObject>(nullptr, *StyleHC);
		if (!OldClass || !NewClass)
			return FMonolithActionResult::Error(TEXT("Failed to resolve normal/high_contrast style class paths"));

		const TArray<FAssetData> Found = GetWbpsInFolder(FolderPath);
		int32 WbpsScanned = 0, WidgetsSwapped = 0;

		for (const FAssetData& AD : Found)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp || !Wbp->WidgetTree) continue;
			WbpsScanned++;

			bool bDirty = false;
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				// Swap on buttons / text / border for their Style properties.
				auto TrySwap = [&](UClass* WClass, UObject* Obj)
				{
					FObjectProperty* P = FindFProperty<FObjectProperty>(WClass, TEXT("Style"));
					if (!P) return;
					UObject* Cur = P->GetObjectPropertyValue_InContainer(Obj);
					if (Cur == OldClass)
					{
						P->SetObjectPropertyValue(P->ContainerPtrToValuePtr<void>(Obj), NewClass);
						WidgetsSwapped++;
						bDirty = true;
					}
				};

				if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(W)) TrySwap(UCommonButtonBase::StaticClass(), Btn);
				if (UCommonTextBlock* Txt = Cast<UCommonTextBlock>(W)) TrySwap(UCommonTextBlock::StaticClass(), Txt);
			});

			if (bDirty)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
				FKismetEditorUtilities::CompileBlueprint(Wbp);
				Wbp->GetOutermost()->MarkPackageDirty();
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("folder_path"), FolderPath);
		Result->SetNumberField(TEXT("wbps_scanned"), WbpsScanned);
		Result->SetNumberField(TEXT("widgets_swapped"), WidgetsSwapped);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("enforce_focus_ring"),
			TEXT("Audit buttons in a folder: report UCommonButtonBase widgets without a style asset. Recommendation for hospice-accessibility focus ring (≥3px)."),
			FMonolithActionHandler::CreateStatic(&HandleEnforceFocusRing),
			FParamSchemaBuilder()
				.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder to scan (e.g. /Game/UI)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("wrap_with_reduce_motion_gate"),
			TEXT("Stamp bRespectReduceMotion=true on WBP CDOs in a folder. WBPs must expose the property + gate animation-triggers on UAccessibilitySubsystem::IsReduceMotionEnabled()."),
			FMonolithActionHandler::CreateStatic(&HandleWrapWithReduceMotionGate),
			FParamSchemaBuilder()
				.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder to scan"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_text_scale_binding"),
			TEXT("Stamp bHonorAccessibilityTextScale=true on WBP CDOs containing UCommonTextBlock widgets. WBPs must multiply font size by accessibility text scale."),
			FMonolithActionHandler::CreateStatic(&HandleSetTextScaleBinding),
			FParamSchemaBuilder()
				.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder to scan"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("apply_high_contrast_variant"),
			TEXT("Swap a normal style class for a high-contrast variant across a folder of WBPs (theme-swap for accessibility mode)"),
			FMonolithActionHandler::CreateStatic(&HandleApplyHighContrastVariant),
			FParamSchemaBuilder()
				.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder to scan"))
				.Required(TEXT("normal_style"), TEXT("string"), TEXT("Class path of normal style to replace"))
				.Required(TEXT("high_contrast_style"), TEXT("string"), TEXT("Class path of high-contrast replacement style"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
