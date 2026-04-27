// Category B: Buttons + Styling — 9 actions
// 2.B.1 convert_button_to_common
// 2.B.2 configure_common_button
// 2.B.3 create_common_button_style
// 2.B.4 create_common_text_style
// 2.B.5 create_common_border_style
// 2.B.6 apply_style_to_widget
// 2.B.7 batch_retheme
// 2.B.8 configure_common_text
// 2.B.9 configure_common_border
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithUICommon.h"

#include "CommonButtonBase.h"
#include "CommonTextBlock.h"
#include "CommonBorder.h"

#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/ContentWidget.h"
#include "Components/PanelSlot.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "UObject/SavePackage.h"

// Phase G: route style-asset creation through the dedup service.
#include "Style/MonolithUIStyleService.h"

namespace MonolithCommonUIButton
{
	// ----- Shared: create-or-resolve a style Blueprint via the Style Service ---
	//
	// CommonUI styles are TSubclassOf<UStyle> — widgets expect a UClass*, not
	// a plain UObject instance. The class-as-data pattern is unchanged, but
	// the asset-creation path now delegates to FMonolithUIStyleService which:
	//   1. Looks up the cache by asset_name (instant return on repeat call).
	//   2. Falls back to a canonical-library scan
	//      (UMonolithUISettings::CanonicalLibraryPath).
	//   3. Falls back to a content-hash cache (catches repeat property bags
	//      submitted under different names — the dedup that matters for LLMs).
	//   4. Finally creates the asset with AssetTools-deduplicated naming so
	//      racing callers don't collide on the same target path.
	//
	// Action surface preserved: same input fields (package_path, asset_name,
	// properties) and same response shape (asset_path, asset_name, class) so
	// callers don't break. Phase G additions (resolved_via, was_created) are
	// purely additive.

	static FMonolithActionResult CreateStyleAsset(
		UClass* StyleClass,
		const TSharedPtr<FJsonObject>& Params)
	{
		FString PackagePath, AssetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("package_path"), PackagePath) ||
			!Params->TryGetStringField(TEXT("asset_name"), AssetName))
			return FMonolithActionResult::Error(TEXT("package_path and asset_name required"));

		if (!StyleClass)
			return FMonolithActionResult::Error(TEXT("CreateStyleAsset: StyleClass is null"));

		// Optional properties bag — passed straight through to the service so
		// it participates in the content hash. Missing bag = empty JSON object
		// (still hashable; produces a stable name suffix).
		TSharedPtr<FJsonObject> Properties;
		const TSharedPtr<FJsonObject>* PropsPtr;
		if (Params->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
		{
			Properties = *PropsPtr;
		}
		else
		{
			Properties = MakeShared<FJsonObject>();
		}

		FUIStyleResolution Resolution = FMonolithUIStyleService::Get().ResolveOrCreate(
			StyleClass, AssetName, PackagePath, Properties);

		if (!Resolution.IsValid())
		{
			return FMonolithActionResult::Error(Resolution.Error.IsEmpty()
				? FString::Printf(TEXT("CreateStyleAsset: style service failed to resolve '%s'"), *AssetName)
				: Resolution.Error);
		}

		// Reconstruct the on-disk asset path from the (possibly deduped) name
		// + folder so callers reading the response can locate the asset.
		const FString ResolvedFullPath = FString::Printf(TEXT("%s/%s"),
			*Resolution.PackagePath, *Resolution.AssetName);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), ResolvedFullPath);
		Result->SetStringField(TEXT("asset_name"), Resolution.AssetName);
		Result->SetStringField(TEXT("class"), Resolution.StyleClass->GetPathName());
		Result->SetStringField(TEXT("resolved_via"), Resolution.ResolvedVia);
		Result->SetBoolField(TEXT("was_created"), Resolution.bWasCreated);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.3 create_common_button_style ------------------------------------

	static FMonolithActionResult HandleCreateCommonButtonStyle(const TSharedPtr<FJsonObject>& Params)
	{
		return CreateStyleAsset(UCommonButtonStyle::StaticClass(), Params);
	}

	// ----- 2.B.4 create_common_text_style --------------------------------------

	static FMonolithActionResult HandleCreateCommonTextStyle(const TSharedPtr<FJsonObject>& Params)
	{
		return CreateStyleAsset(UCommonTextStyle::StaticClass(), Params);
	}

	// ----- 2.B.5 create_common_border_style ------------------------------------

	static FMonolithActionResult HandleCreateCommonBorderStyle(const TSharedPtr<FJsonObject>& Params)
	{
		return CreateStyleAsset(UCommonBorderStyle::StaticClass(), Params);
	}

	// ----- 2.B.6 apply_style_to_widget -----------------------------------------
	//
	// Phase G note: this action consumes a fully-resolved style class path and
	// assigns it to a widget — it is the read-side counterpart to
	// create_common_*_style. Style-asset creation goes through the service via
	// CreateStyleAsset (above); this action only LoadClass's the resulting _C
	// path. No service call is needed here because the path already encodes the
	// resolution decision the service made when the style was created.

	static FMonolithActionResult HandleApplyStyleToWidget(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, StyleAssetPath;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName) ||
			!Params->TryGetStringField(TEXT("style_asset"), StyleAssetPath))
			return FMonolithActionResult::Error(TEXT("wbp_path, widget_name, style_asset required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		// The style asset path usually refers to a BLUEPRINT-derived style class, not the base asset.
		// Convention: user passes the class path (/Game/UI/Styles/BS_Primary.BS_Primary_C).
		UClass* StyleClass = LoadClass<UObject>(nullptr, *StyleAssetPath);
		if (!StyleClass)
		{
			// Fallback: load as asset and GeneratedClass
			UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *StyleAssetPath);
			if (BP) StyleClass = BP->GeneratedClass;
		}
		if (!StyleClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to resolve style class '%s'"), *StyleAssetPath));

		FString AppliedVia;

		if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(Target))
		{
			if (!StyleClass->IsChildOf(UCommonButtonStyle::StaticClass()))
				return FMonolithActionResult::Error(TEXT("style_asset is not a UCommonButtonStyle"));
			Btn->SetStyle(StyleClass);
			AppliedVia = TEXT("UCommonButtonBase::SetStyle");
		}
		else if (UCommonTextBlock* Txt = Cast<UCommonTextBlock>(Target))
		{
			if (!StyleClass->IsChildOf(UCommonTextStyle::StaticClass()))
				return FMonolithActionResult::Error(TEXT("style_asset is not a UCommonTextStyle"));
			Txt->SetStyle(StyleClass);
			AppliedVia = TEXT("UCommonTextBlock::SetStyle");
		}
		else if (UCommonBorder* Brd = Cast<UCommonBorder>(Target))
		{
			if (!StyleClass->IsChildOf(UCommonBorderStyle::StaticClass()))
				return FMonolithActionResult::Error(TEXT("style_asset is not a UCommonBorderStyle"));
			Brd->SetStyle(StyleClass);
			AppliedVia = TEXT("UCommonBorder::SetStyle");
		}
		else
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' is not a button/text/border — cannot apply style"), *WidgetName));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("style_class"), StyleClass->GetName());
		Result->SetStringField(TEXT("applied_via"), AppliedVia);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.2 configure_common_button ---------------------------------------

	static FMonolithActionResult HandleConfigureCommonButton(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonButtonBase* Btn = Cast<UCommonButtonBase>(Target);
		if (!Btn)
			return FMonolithActionResult::Error(TEXT("target is not a UCommonButtonBase"));

		TArray<FString> Applied;

		bool Bv;
		if (Params->TryGetBoolField(TEXT("is_toggleable"), Bv))
		{
			Btn->SetIsToggleable(Bv);
			Applied.Add(TEXT("is_toggleable"));
		}
		if (Params->TryGetBoolField(TEXT("requires_hold"), Bv))
		{
			Btn->SetRequiresHold(Bv);
			Applied.Add(TEXT("requires_hold"));
		}

		int32 MinW = 0, MinH = 0;
		const bool bHasMinW = Params->TryGetNumberField(TEXT("min_width"), MinW);
		const bool bHasMinH = Params->TryGetNumberField(TEXT("min_height"), MinH);
		if (bHasMinW || bHasMinH)
		{
			Btn->SetMinDimensions(MinW, MinH);
			Applied.Add(TEXT("min_dimensions"));
		}
		int32 MaxW = 0, MaxH = 0;
		const bool bHasMaxW = Params->TryGetNumberField(TEXT("max_width"), MaxW);
		const bool bHasMaxH = Params->TryGetNumberField(TEXT("max_height"), MaxH);
		if (bHasMaxW || bHasMaxH)
		{
			Btn->SetMaxDimensions(MaxW, MaxH);
			Applied.Add(TEXT("max_dimensions"));
		}

		FString ClickMethodStr;
		if (Params->TryGetStringField(TEXT("click_method"), ClickMethodStr))
		{
			EButtonClickMethod::Type Method = EButtonClickMethod::DownAndUp;
			if (ClickMethodStr.Equals(TEXT("MouseDown"), ESearchCase::IgnoreCase)) Method = EButtonClickMethod::MouseDown;
			else if (ClickMethodStr.Equals(TEXT("MouseUp"), ESearchCase::IgnoreCase)) Method = EButtonClickMethod::MouseUp;
			else if (ClickMethodStr.Equals(TEXT("PreciseClick"), ESearchCase::IgnoreCase)) Method = EButtonClickMethod::PreciseClick;
			Btn->SetClickMethod(Method);
			Applied.Add(TEXT("click_method"));
		}

		FString DisabledReason;
		if (Params->TryGetStringField(TEXT("disabled_reason"), DisabledReason) && !DisabledReason.IsEmpty())
		{
			Btn->DisableButtonWithReason(FText::FromString(DisabledReason));
			Applied.Add(TEXT("disabled_reason"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) AppliedArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("applied"), AppliedArr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.1 convert_button_to_common --------------------------------------

	static FMonolithActionResult HandleConvertButtonToCommon(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		// Optional: caller supplies a concrete UCommonButtonBase subclass to construct.
		// UCommonButtonBase itself is UCLASS(Abstract) in UE 5.7, so defaulting to the base
		// produces a null widget and silently drops the button from the tree. Reject that path.
		FString TargetClassName;
		Params->TryGetStringField(TEXT("target_class"), TargetClassName);

		UClass* TargetClass = nullptr;
		if (!TargetClassName.IsEmpty())
		{
			TargetClass = FindFirstObject<UClass>(*TargetClassName, EFindFirstObjectOptions::NativeFirst);
			if (!TargetClass)
			{
				TargetClass = FindFirstObject<UClass>(*(TEXT("U") + TargetClassName), EFindFirstObjectOptions::NativeFirst);
			}
			if (!TargetClass)
			{
				TargetClass = LoadClass<UObject>(nullptr, *TargetClassName);
			}
			if (!TargetClass)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("target_class '%s' could not be resolved. Use full path (/Script/Module.ClassName) or a loaded class name."),
					*TargetClassName));
			}
			if (!TargetClass->IsChildOf(UCommonButtonBase::StaticClass()))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("target_class '%s' is not a UCommonButtonBase subclass"), *TargetClassName));
			}
		}
		else
		{
			// No target_class specified — create a persistent concrete Blueprint
			// subclass of UCommonButtonBase on demand. Cached per-session so we
			// only create it once. Cannot use a UCLASS here because UHT rejects
			// UCLASS inside #if WITH_COMMONUI preprocessor blocks.
			// Must live in a real package (not transient) so WBPs referencing
			// the generated class can save to disk.
			static TWeakObjectPtr<UClass> CachedDefaultClass;
			if (!CachedDefaultClass.IsValid())
			{
				const FString DefaultPath = TEXT("/Game/Monolith/CommonUI/MonolithDefaultCommonButton");
				// Check if it already exists on disk from a prior session
				UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *DefaultPath);
				if (!BP)
				{
					UPackage* Pkg = CreatePackage(*DefaultPath);
					if (Pkg)
					{
						BP = FKismetEditorUtilities::CreateBlueprint(
							UCommonButtonBase::StaticClass(),
							Pkg,
							TEXT("MonolithDefaultCommonButton"),
							BPTYPE_Normal,
							UBlueprint::StaticClass(),
							UBlueprintGeneratedClass::StaticClass());
						if (BP)
						{
							FKismetEditorUtilities::CompileBlueprint(BP);
							FAssetRegistryModule::AssetCreated(BP);
							Pkg->MarkPackageDirty();
							FSavePackageArgs SaveArgs;
							SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
							UPackage::SavePackage(Pkg, BP,
								*FPackageName::LongPackageNameToFilename(DefaultPath, FPackageName::GetAssetPackageExtension()),
								SaveArgs);
						}
					}
				}
				if (BP && BP->GeneratedClass)
				{
					CachedDefaultClass = BP->GeneratedClass;
				}
			}
			TargetClass = CachedDefaultClass.Get();
			if (!TargetClass)
				return FMonolithActionResult::Error(TEXT("Failed to create default CommonButton subclass. Pass a concrete target_class explicitly."));
		}

		if (TargetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("target_class '%s' is abstract/deprecated — cannot construct. Pass a concrete subclass via the 'target_class' param."),
				*TargetClass->GetName()));
		}

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UButton* OldBtn = Cast<UButton>(Target);
		if (!OldBtn)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' is not a UButton"), *WidgetName));

		UPanelSlot* ParentSlot = OldBtn->Slot;
		UPanelWidget* Parent = ParentSlot ? ParentSlot->Parent : nullptr;
		if (!Parent)
			return FMonolithActionResult::Error(TEXT("Cannot convert root-level button — parent required for reparent"));

		// Capture the single child if any (UButton is a UContentWidget)
		FString ChildName;
		if (OldBtn->GetChildrenCount() > 0)
		{
			if (UWidget* Child = OldBtn->GetChildAt(0))
			{
				ChildName = Child->GetName();
			}
		}

		// Create the new common button — preserve name
		const FName BtnName = OldBtn->GetFName();
		const FName TempBtnName = MakeUniqueObjectName(
			Wbp->WidgetTree,
			TargetClass,
			FName(*FString::Printf(TEXT("%s_CommonReplacement"), *BtnName.ToString())));

		UCommonButtonBase* NewBtn = Wbp->WidgetTree->ConstructWidget<UCommonButtonBase>(TargetClass, TempBtnName);
		if (!NewBtn)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("ConstructWidget returned null for class '%s' (abstract or failed instantiation). "
					 "Pass a verified concrete 'target_class'."),
				*TargetClass->GetName()));
		}

		Parent->RemoveChild(OldBtn);
		OldBtn->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
		NewBtn->Rename(*BtnName.ToString(), Wbp->WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty);
		Parent->AddChild(NewBtn);
		MonolithUI::RegisterCreatedWidget(Wbp, NewBtn);

		// Note: UCommonButtonBase is a UCommonUserWidget (not a UPanelWidget), so its content
		// tree is internal rather than a single AddChild slot like UButton. Children from the
		// original UButton must be rewired manually by the author — we cannot auto-transfer.
		// ReconcileWidgetVariableGuids prunes removed child names without invoking the broader
		// editor delete path, which would also strip graph references to the replaced button.

		MonolithUI::ReconcileWidgetVariableGuids(Wbp);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), BtnName.ToString());
		Result->SetStringField(TEXT("new_class"), TargetClass->GetName());
		Result->SetBoolField(TEXT("had_child"), !ChildName.IsEmpty());
		if (!ChildName.IsEmpty())
		{
			Result->SetStringField(TEXT("removed_child"), ChildName);
			Result->SetStringField(TEXT("orphaned_child"), ChildName);
			Result->SetStringField(TEXT("note"), TEXT("Old UButton child not auto-transferred — UCommonButtonBase uses internal widget tree, not AddChild. Rewire manually."));
		}
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.7 batch_retheme -------------------------------------------------
	//
	// Phase G note: batch_retheme operates on TWO existing style class paths
	// (old + new) and rewrites widget references across a folder of WBPs. Both
	// inputs are pre-resolved; the service does not participate in this hot
	// path. If a future variant accepts a style PROPERTIES bag instead of a
	// pre-built class, route the bag through FMonolithUIStyleService::Get()
	// .ResolveOrCreate to get back the class to apply — same pattern as
	// CreateStyleAsset above.

	static FMonolithActionResult HandleBatchRetheme(const TSharedPtr<FJsonObject>& Params)
	{
		FString FolderPath, StyleOldPath, StyleNewPath;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("folder_path"), FolderPath) ||
			!Params->TryGetStringField(TEXT("old_style"), StyleOldPath) ||
			!Params->TryGetStringField(TEXT("new_style"), StyleNewPath))
			return FMonolithActionResult::Error(TEXT("folder_path, old_style, new_style required"));

		// Resolve style class — try direct LoadClass first, then Blueprint fallback
		auto ResolveStyleClass = [](const FString& Path) -> UClass*
		{
			UClass* C = LoadClass<UObject>(nullptr, *Path);
			if (!C)
			{
				UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
				if (BP) C = BP->GeneratedClass;
			}
			return C;
		};
		UClass* OldClass = ResolveStyleClass(StyleOldPath);
		UClass* NewClass = ResolveStyleClass(StyleNewPath);
		if (!OldClass || !NewClass)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to resolve style class paths. old='%s' (%s), new='%s' (%s)"),
				*StyleOldPath, OldClass ? TEXT("OK") : TEXT("FAILED"),
				*StyleNewPath, NewClass ? TEXT("OK") : TEXT("FAILED")));

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
		Filter.PackagePaths.Add(FName(*FolderPath));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> FoundAssets;
		ARM.Get().GetAssets(Filter, FoundAssets);

		int32 WbpsScanned = 0, WidgetsRethemed = 0;

		for (const FAssetData& AD : FoundAssets)
		{
			UWidgetBlueprint* Wbp = Cast<UWidgetBlueprint>(AD.GetAsset());
			if (!Wbp || !Wbp->WidgetTree) continue;
			WbpsScanned++;

			bool bDirty = false;
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (UCommonButtonBase* Btn = Cast<UCommonButtonBase>(W))
				{
					// Access current style via reflection (Style property exists on UCommonButtonBase)
					FObjectProperty* StyleProp = FindFProperty<FObjectProperty>(UCommonButtonBase::StaticClass(), TEXT("Style"));
					if (StyleProp)
					{
						UObject* Cur = StyleProp->GetObjectPropertyValue_InContainer(Btn);
						if (Cur == OldClass)
						{
							Btn->SetStyle(NewClass);
							bDirty = true;
							WidgetsRethemed++;
						}
					}
				}
				else if (UCommonTextBlock* Txt = Cast<UCommonTextBlock>(W))
				{
					FObjectProperty* StyleProp = FindFProperty<FObjectProperty>(UCommonTextBlock::StaticClass(), TEXT("Style"));
					if (StyleProp)
					{
						UObject* Cur = StyleProp->GetObjectPropertyValue_InContainer(Txt);
						if (Cur == OldClass)
						{
							Txt->SetStyle(NewClass);
							bDirty = true;
							WidgetsRethemed++;
						}
					}
				}
				else if (UCommonBorder* Brd = Cast<UCommonBorder>(W))
				{
					FObjectProperty* StyleProp = FindFProperty<FObjectProperty>(UCommonBorder::StaticClass(), TEXT("Style"));
					if (StyleProp)
					{
						UObject* Cur = StyleProp->GetObjectPropertyValue_InContainer(Brd);
						if (Cur == OldClass)
						{
							Brd->SetStyle(NewClass);
							bDirty = true;
							WidgetsRethemed++;
						}
					}
				}
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
		Result->SetNumberField(TEXT("widgets_rethemed"), WidgetsRethemed);
		Result->SetStringField(TEXT("old_style"), StyleOldPath);
		Result->SetStringField(TEXT("new_style"), StyleNewPath);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.8 configure_common_text -----------------------------------------

	static FMonolithActionResult HandleConfigureCommonText(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonTextBlock* Txt = Cast<UCommonTextBlock>(Target);
		if (!Txt)
			return FMonolithActionResult::Error(TEXT("target is not a UCommonTextBlock"));

		TArray<FString> Applied;

		double NumVal;
		if (Params->TryGetNumberField(TEXT("wrap_text_width"), NumVal))
		{
			Txt->SetWrapTextWidth(static_cast<int32>(NumVal));
			Applied.Add(TEXT("wrap_text_width"));
		}
		if (Params->TryGetNumberField(TEXT("line_height_percentage"), NumVal))
		{
			Txt->SetLineHeightPercentage(static_cast<float>(NumVal));
			Applied.Add(TEXT("line_height_percentage"));
		}
		if (Params->TryGetNumberField(TEXT("mobile_font_size_multiplier"), NumVal))
		{
			Txt->SetMobileFontSizeMultiplier(static_cast<float>(NumVal));
			Applied.Add(TEXT("mobile_font_size_multiplier"));
		}

		bool Bv;
		if (Params->TryGetBoolField(TEXT("scrolling_enabled"), Bv))
		{
			Txt->SetScrollingEnabled(Bv);
			Applied.Add(TEXT("scrolling_enabled"));
		}

		FString TextCaseStr;
		if (Params->TryGetStringField(TEXT("text_case"), TextCaseStr))
		{
			ETextTransformPolicy TCase = ETextTransformPolicy::None;
			if (TextCaseStr.Equals(TEXT("ToUpper"), ESearchCase::IgnoreCase)) TCase = ETextTransformPolicy::ToUpper;
			else if (TextCaseStr.Equals(TEXT("ToLower"), ESearchCase::IgnoreCase)) TCase = ETextTransformPolicy::ToLower;
			Txt->SetTextTransformPolicy(TCase);
			Applied.Add(TEXT("text_case"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) AppliedArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("applied"), AppliedArr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- 2.B.9 configure_common_border ---------------------------------------

	static FMonolithActionResult HandleConfigureCommonBorder(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and widget_name required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonBorder* Brd = Cast<UCommonBorder>(Target);
		if (!Brd)
			return FMonolithActionResult::Error(TEXT("target is not a UCommonBorder"));

		TArray<FString> Applied;

		bool Bv;
		if (Params->TryGetBoolField(TEXT("reduce_padding_by_safezone"), Bv))
		{
			if (FBoolProperty* P = FindFProperty<FBoolProperty>(UCommonBorder::StaticClass(), TEXT("bReducePaddingBySafezone")))
			{
				P->SetPropertyValue_InContainer(Brd, Bv);
				Applied.Add(TEXT("reduce_padding_by_safezone"));
			}
		}

		// MinimumPadding — FMargin via text import
		FString MinPadText;
		if (Params->TryGetStringField(TEXT("minimum_padding"), MinPadText))
		{
			if (FStructProperty* P = FindFProperty<FStructProperty>(UCommonBorder::StaticClass(), TEXT("MinimumPadding")))
			{
				P->ImportText_Direct(*MinPadText, P->ContainerPtrToValuePtr<void>(Brd), nullptr, PPF_None);
				Applied.Add(TEXT("minimum_padding"));
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		TArray<TSharedPtr<FJsonValue>> AppliedArr;
		for (const FString& A : Applied) AppliedArr.Add(MakeShared<FJsonValueString>(A));
		Result->SetArrayField(TEXT("applied"), AppliedArr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("convert_button_to_common"),
			TEXT("Replace a UButton in a WBP with a UCommonButtonBase-derived class, preserving name and parent. "
				 "Creates a transient concrete subclass by default if target_class is omitted. "
				 "Override via 'target_class' for project-specific subclasses. Old UButton child is NOT auto-transferred."),
			FMonolithActionHandler::CreateStatic(&HandleConvertButtonToCommon),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Target Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the UButton to convert"))
				.Optional(TEXT("target_class"), TEXT("string"),
					TEXT("Concrete UCommonButtonBase subclass to construct. Use /Script/Module.ClassName or a loaded class name. "
						 "Omit to auto-create a transient concrete subclass."))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("configure_common_button"),
			TEXT("Set UCommonButtonBase properties: toggle, hold, dimensions, click method, disabled reason"),
			FMonolithActionHandler::CreateStatic(&HandleConfigureCommonButton),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the UCommonButtonBase"))
				.Optional(TEXT("is_toggleable"), TEXT("boolean"), TEXT("Enable toggle behavior"))
				.Optional(TEXT("requires_hold"), TEXT("boolean"), TEXT("Require hold-to-confirm"))
				.Optional(TEXT("min_width"), TEXT("integer"), TEXT("Minimum width (px)"))
				.Optional(TEXT("min_height"), TEXT("integer"), TEXT("Minimum height (px)"))
				.Optional(TEXT("max_width"), TEXT("integer"), TEXT("Maximum width (px)"))
				.Optional(TEXT("max_height"), TEXT("integer"), TEXT("Maximum height (px)"))
				.Optional(TEXT("click_method"), TEXT("string"), TEXT("DownAndUp / MouseDown / MouseUp / PreciseClick"))
				.Optional(TEXT("disabled_reason"), TEXT("string"), TEXT("If non-empty, disable button with this reason"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_common_button_style"),
			TEXT("Create a UCommonButtonStyle Blueprint class (class-as-data). Properties applied to CDO via reflection. Returns the _C class path for use with apply_style_to_widget."),
			FMonolithActionHandler::CreateStatic(&HandleCreateCommonButtonStyle),
			FParamSchemaBuilder()
				.Required(TEXT("package_path"), TEXT("string"), TEXT("Folder, e.g. /Game/UI/Styles"))
				.Required(TEXT("asset_name"), TEXT("string"), TEXT("Asset name"))
				.Optional(TEXT("properties"), TEXT("object"), TEXT("Initial property values (reflection-assigned)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_common_text_style"),
			TEXT("Create a UCommonTextStyle Blueprint class (class-as-data). Properties applied to CDO via reflection. Returns the _C class path for use with apply_style_to_widget."),
			FMonolithActionHandler::CreateStatic(&HandleCreateCommonTextStyle),
			FParamSchemaBuilder()
				.Required(TEXT("package_path"), TEXT("string"), TEXT("Folder path"))
				.Required(TEXT("asset_name"), TEXT("string"), TEXT("Asset name"))
				.Optional(TEXT("properties"), TEXT("object"), TEXT("Initial property values"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("create_common_border_style"),
			TEXT("Create a UCommonBorderStyle Blueprint class (class-as-data). Properties applied to CDO via reflection. Returns the _C class path for use with apply_style_to_widget."),
			FMonolithActionHandler::CreateStatic(&HandleCreateCommonBorderStyle),
			FParamSchemaBuilder()
				.Required(TEXT("package_path"), TEXT("string"), TEXT("Folder path"))
				.Required(TEXT("asset_name"), TEXT("string"), TEXT("Asset name"))
				.Optional(TEXT("properties"), TEXT("object"), TEXT("Initial property values"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("apply_style_to_widget"),
			TEXT("Assign a UCommonButtonStyle / UCommonTextStyle / UCommonBorderStyle class to a widget in a WBP"),
			FMonolithActionHandler::CreateStatic(&HandleApplyStyleToWidget),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of widget to style"))
				.Required(TEXT("style_asset"), TEXT("string"), TEXT("Style class path (usually ends with _C)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("batch_retheme"),
			TEXT("Scan a folder of WBPs and swap one style class reference for another (theme-swap)"),
			FMonolithActionHandler::CreateStatic(&HandleBatchRetheme),
			FParamSchemaBuilder()
				.Required(TEXT("folder_path"), TEXT("string"), TEXT("Folder to scan (e.g. /Game/UI)"))
				.Required(TEXT("old_style"), TEXT("string"), TEXT("Old style class path"))
				.Required(TEXT("new_style"), TEXT("string"), TEXT("New style class path"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("configure_common_text"),
			TEXT("Configure UCommonTextBlock: wrap, case, line-height, scroll, mobile multiplier"),
			FMonolithActionHandler::CreateStatic(&HandleConfigureCommonText),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of UCommonTextBlock"))
				.Optional(TEXT("wrap_text_width"), TEXT("number"), TEXT("Wrap width (px)"))
				.Optional(TEXT("line_height_percentage"), TEXT("number"), TEXT("Line height as fraction (1.0 = default)"))
				.Optional(TEXT("mobile_font_size_multiplier"), TEXT("number"), TEXT("Scale factor on mobile"))
				.Optional(TEXT("scrolling_enabled"), TEXT("boolean"), TEXT("Enable marquee scroll"))
				.Optional(TEXT("text_case"), TEXT("string"), TEXT("None / ToUpper / ToLower"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("configure_common_border"),
			TEXT("Configure UCommonBorder: reduce_padding_by_safezone, minimum_padding"),
			FMonolithActionHandler::CreateStatic(&HandleConfigureCommonBorder),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of UCommonBorder"))
				.Optional(TEXT("reduce_padding_by_safezone"), TEXT("boolean"), TEXT("Honor platform safe-zone"))
				.Optional(TEXT("minimum_padding"), TEXT("string"), TEXT("FMargin text format, e.g. '(Left=0,Top=0,Right=0,Bottom=0)'"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
