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

// Phase 2 (2026-05-16 UI gap audit) — Items #10, #12, #13.
// Item #10 probes Tokenforge availability through IPluginManager (Projects module).
// Item #12 mirrors convert_button_to_common's variable-reconciliation choreography
//   on UTextBlock -> UCommonTextBlock.
// Item #13 needs UWidgetBlueprintGeneratedClass + UCommonBoundActionBar for the
//   archetype-write that survives compile_blueprint.
#include "Interfaces/IPluginManager.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Input/CommonBoundActionBar.h"

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

	// Phase 4 Item #19 (2026-05-16 UI gap audit): when caller omits `target_class`
	// AND TokenforgeRuntime is installed + enabled, prefer Tokenforge's
	// `TokenforgeCommonButton` over the transient `MonolithDefaultCommonButton`
	// fallback. Probe is reflective (no compile-time dep on Tokenforge) and
	// gracefully degrades when the plugin / class is absent. Caller-supplied
	// target_class always wins — this helper only fires on the no-target path.
	static FString ResolveDefaultButtonClass()
	{
		if (IPluginManager::Get().FindPlugin(TEXT("TokenforgeRuntime")).IsValid()
			&& IPluginManager::Get().FindPlugin(TEXT("TokenforgeRuntime"))->IsEnabled())
		{
			if (UClass* TokenforgeButtonClass = FindFirstObject<UClass>(TEXT("TokenforgeCommonButton")))
				return TokenforgeButtonClass->GetPathName();
		}
		return TEXT("/Game/Monolith/CommonUI/MonolithDefaultCommonButton.MonolithDefaultCommonButton_C");
	}

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
			// Phase 4 Item #19 (2026-05-16 UI gap audit): probe TokenforgeRuntime
			// FIRST. If installed + enabled AND a TokenforgeCommonButton class is
			// resolvable, use it. The transient `MonolithDefaultCommonButton`
			// fallback below stays in place for the Tokenforge-absent case.
			const FString ResolvedDefault = ResolveDefaultButtonClass();
			if (!ResolvedDefault.StartsWith(TEXT("/Game/Monolith/CommonUI/MonolithDefaultCommonButton")))
			{
				if (UClass* TokenforgeClass = LoadClass<UObject>(nullptr, *ResolvedDefault))
				{
					if (TokenforgeClass->IsChildOf(UCommonButtonBase::StaticClass())
						&& !TokenforgeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
					{
						TargetClass = TokenforgeClass;
					}
				}
			}

			// Fallback (Phase 4 Item #19): only run the persistent
			// MonolithDefaultCommonButton creation path if the Tokenforge probe
			// above did NOT resolve a class. Gated by `TargetClass == nullptr`.
			if (!TargetClass)
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

		TArray<UWidget*> OldButtonChildren;
		for (int32 ChildIndex = 0; ChildIndex < OldBtn->GetChildrenCount(); ++ChildIndex)
		{
			if (UWidget* ChildWidget = OldBtn->GetChildAt(ChildIndex))
			{
				OldButtonChildren.AddUnique(ChildWidget);
				TArray<UWidget*> Descendants;
				UWidgetTree::GetChildWidgets(ChildWidget, Descendants);
				for (UWidget* Descendant : Descendants)
				{
					OldButtonChildren.AddUnique(Descendant);
				}
			}
		}

		OldBtn->Modify();
		OldBtn->ClearChildren();
		for (UWidget* ChildWidget : OldButtonChildren)
		{
			if (!ChildWidget)
			{
				continue;
			}

			const FName ChildWidgetName = ChildWidget->GetFName();
			for (int32 BindingIndex = Wbp->Bindings.Num() - 1; BindingIndex >= 0; --BindingIndex)
			{
				if (Wbp->Bindings[BindingIndex].ObjectName == ChildWidgetName.ToString())
				{
					Wbp->Bindings.RemoveAt(BindingIndex);
				}
			}

			ChildWidget->Modify();
			ChildWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			Wbp->OnVariableRemoved(ChildWidgetName);
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

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		MonolithUI::ReconcileWidgetVariableGuids(Wbp);
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

	// ----- Phase 2 Item #10 — apply_token_binding ------------------------------
	//
	// Tokenforge-backed style binding. The full implementation programmatically
	// writes BP-graph nodes into the WBP's NativeConstruct event graph so the
	// widget calls UUISubsystem::GetColor / GetFont / etc. at construct time and
	// pipes the result into a target property's setter.
	//
	// PHASE 2 IMPLEMENTATION LEVEL: MVP-STUB.
	//
	// Why stub: writing K2 node graphs programmatically (UK2Node_CallFunction,
	// UK2Node_VariableSet, UK2Node_Knot for routing, ULinker::Resolve for the
	// pin schema) is non-trivial — the canonical surface lives in
	// MonolithBlueprint/Private/MonolithBlueprintNodeActions.cpp and would
	// require either (a) cross-module include of those helpers (currently NOT
	// in MonolithUI.Build.cs PrivateDependencyModuleNames) or (b) re-implementing
	// the K2Node construction + pin-wiring inside MonolithUI. Either path
	// inflates this dispatch beyond the time budget; the action is registered
	// here so downstream callers don't 404, the Tokenforge availability probe
	// works end-to-end, and a follow-up issue can land the BP-graph node-write
	// surface.
	//
	// Tokenforge probe + -32011 error code path are FULLY implemented per the
	// design spec — that's the critical bit for the Steam build's "optional dep
	// absent" telemetry. The BP-graph node-write half is deferred.

	static FMonolithActionResult HandleApplyTokenBinding(const TSharedPtr<FJsonObject>& Params)
	{
		// --- Tokenforge availability probe (FULL impl) --------------------------
		// Mirrors the -32010 EffectSurface pattern from SPEC_MonolithUI §
		// "Error Contract — Optional EffectSurface Provider Absence (-32010)".
		// -32011 is the next reserved slot from the JSON-RPC server-defined
		// range (-32011..-32019 left open per MonolithJsonUtils.h:50).
		TSharedPtr<IPlugin> TokenforgePlugin =
			IPluginManager::Get().FindPlugin(TEXT("TokenforgeRuntime"));
		const bool bTokenforgeAvailable = TokenforgePlugin.IsValid() && TokenforgePlugin->IsEnabled();

		if (!bTokenforgeAvailable)
		{
			// Same shape as MakeOptionalDepUnavailableError but using -32011 so
			// the LLM can branch on "missing provider == Tokenforge" without
			// string-matching the message.
			FMonolithActionResult Err = FMonolithActionResult::Error(
				TEXT("apply_token_binding unavailable — TokenforgeRuntime plugin not enabled. "
					 "Install/enable the plugin in <Project>.uproject, or use create_common_*_style "
					 "for project-static style classes instead."),
				-32011);

			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("dep_name"),    TEXT("TokenforgeRuntime"));
			Payload->SetStringField(TEXT("widget_type"), TEXT("ApplyTokenBinding"));
			Payload->SetStringField(TEXT("alternative"), TEXT("create_common_*_style + apply_style_to_widget"));
			Payload->SetStringField(TEXT("category"),    TEXT("OptionalDepUnavailable"));
			Err.Result = Payload;
			return Err;
		}

		// --- Param validation (FULL impl) --------------------------------------
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		FString WidgetName, TargetProperty, TokenKey;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("widget_name"), WidgetName)
			|| !Params->TryGetStringField(TEXT("target_property"), TargetProperty)
			|| !Params->TryGetStringField(TEXT("token_key"), TokenKey))
		{
			return FMonolithActionResult::Error(TEXT("widget_name, target_property, token_key required"));
		}

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		// Verify the target_property actually exists on the widget class — this
		// catches typos before we ship the stub response, so a follow-up
		// full-impl can rely on the validated property path.
		if (!FindFProperty<FProperty>(Target->GetClass(), FName(*TargetProperty)))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Property '%s' not found on widget class '%s'"),
					*TargetProperty, *Target->GetClass()->GetName()),
				-32602);
		}

		// --- MVP-STUB response --------------------------------------------------
		// Action registered, params validated, Tokenforge probe ran. The actual
		// BP-graph node-write into NativeConstruct is deferred — flagged with a
		// machine-readable status field so callers know the difference between
		// "everything wired" and "registered but not yet binding".
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("target_property"), TargetProperty);
		Result->SetStringField(TEXT("token_key"), TokenKey);
		Result->SetBoolField(TEXT("tokenforge_available"), true);
		Result->SetStringField(TEXT("tokenforge_version"), TokenforgePlugin->GetDescriptor().VersionName);
		Result->SetStringField(TEXT("status"), TEXT("stub"));
		Result->SetStringField(TEXT("reason"),
			TEXT("BP-graph node-write surface deferred. Param validation + Tokenforge probe FULL — "
				 "node construction in NativeConstruct event graph awaits issue #2-10b follow-up. "
				 "Action is registered + discoverable so callers can branch on status='stub'."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- Phase 2 Item #12 — convert_textblock_to_common ----------------------
	//
	// Mirrors convert_button_to_common semantics on UTextBlock -> UCommonTextBlock.
	// Steps:
	//   1. Locate the UTextBlock in the WBP tree.
	//   2. Capture text/font/colour state from the leaf widget.
	//   3. Construct a UCommonTextBlock with a temp name (the engine refuses
	//      same-name in same outer for the rename swap).
	//   4. Retire the old widget from its parent + the WBP's Bindings / variables
	//      arrays. UTextBlock is a leaf (no children), so the choreography is
	//      simpler than convert_button_to_common's parent-with-child case.
	//   5. Rename the new widget to the captured FName + AddChild back into the
	//      same parent slot.
	//   6. Replay captured state onto the new widget.
	//   7. Reconcile + compile.
	//
	// Variable identity preserved (Y): we keep the FName via the rename + ensure
	// the variable GUID reconciliation pass runs so the Skeleton class's BPVAR
	// entry matches.

	static FMonolithActionResult HandleConvertTextBlockToCommon(const TSharedPtr<FJsonObject>& Params)
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

		UTextBlock* OldTxt = Cast<UTextBlock>(Target);
		if (!OldTxt)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' is not a UTextBlock"), *WidgetName));

		UPanelSlot* ParentSlot = OldTxt->Slot;
		UPanelWidget* Parent = ParentSlot ? ParentSlot->Parent : nullptr;
		if (!Parent)
			return FMonolithActionResult::Error(TEXT("Cannot convert root-level text block — parent required for reparent"));

		// ----- Capture old state -----------------------------------------------
		const FText          CapturedText      = OldTxt->GetText();
		const FSlateFontInfo CapturedFont      = OldTxt->GetFont();
		const FSlateColor    CapturedColor     = OldTxt->GetColorAndOpacity();
		// UTextBlock::GetShadowColorAndOpacity returns FLinearColor (TextBlock.cpp:82),
		// not FSlateColor. SetShadowColorAndOpacity takes FLinearColor directly.
		const FLinearColor   CapturedShadow    = OldTxt->GetShadowColorAndOpacity();
		const FVector2D      CapturedShadowOff = OldTxt->GetShadowOffset();
		const bool           bWasBoundAsVariable = OldTxt->bIsVariable;

		const FName TxtName = OldTxt->GetFName();
		const FName TempName = MakeUniqueObjectName(
			Wbp->WidgetTree,
			UCommonTextBlock::StaticClass(),
			FName(*FString::Printf(TEXT("%s_CommonReplacement"), *TxtName.ToString())));

		UCommonTextBlock* NewTxt = Wbp->WidgetTree->ConstructWidget<UCommonTextBlock>(
			UCommonTextBlock::StaticClass(), TempName);
		if (!NewTxt)
			return FMonolithActionResult::Error(TEXT("ConstructWidget<UCommonTextBlock> returned null"));

		// ----- Retire old widget -----------------------------------------------
		// IMPORTANT: do NOT call Wbp->OnVariableRemoved(TxtName) for the OLD
		// widget — that would strip any graph references that name the variable,
		// orphaning the substitution before we re-promote the NewTxt under the
		// same FName. Mirrors convert_button_to_common's choreography
		// (line ~477-479): rename old-to-transient, then rename new-to-old-name,
		// and let ReconcileWidgetVariableGuids fix up the BPVAR side.
		OldTxt->Modify();
		Parent->RemoveChild(OldTxt);
		OldTxt->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);

		// ----- Promote new widget to old name + reattach -----------------------
		NewTxt->Rename(*TxtName.ToString(), Wbp->WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty);
		Parent->AddChild(NewTxt);
		MonolithUI::RegisterCreatedWidget(Wbp, NewTxt);
		NewTxt->bIsVariable = bWasBoundAsVariable;

		// ----- Replay captured state -------------------------------------------
		NewTxt->SetText(CapturedText);
		NewTxt->SetFont(CapturedFont);
		NewTxt->SetColorAndOpacity(CapturedColor);
		NewTxt->SetShadowColorAndOpacity(CapturedShadow);
		NewTxt->SetShadowOffset(CapturedShadowOff);

		// ----- Reconcile + compile ---------------------------------------------
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		MonolithUI::ReconcileWidgetVariableGuids(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), TxtName.ToString());
		Result->SetStringField(TEXT("new_class"), TEXT("CommonTextBlock"));
		Result->SetBoolField(TEXT("was_bound_as_variable"), bWasBoundAsVariable);
		Result->SetStringField(TEXT("captured_text"), CapturedText.ToString());
		Result->SetStringField(TEXT("note"),
			TEXT("Variable identity preserved (FName + bIsVariable flag). "
				 "Style left at engine default — call apply_style_to_widget with a UCommonTextStyle next."));
		return FMonolithActionResult::Success(Result);
	}

	// ----- Phase 2 Item #13 — set_action_bar_button_class ----------------------
	//
	// Sets UCommonBoundActionBar::ActionButtonClass on a bar widget that ALREADY
	// exists in a WBP. Mirrors the FClassProperty reflection-write pattern from
	// Phase 1 Bug #4 (MonolithCommonUIInputActions.cpp:265-282) but additionally
	// writes through UWidgetBlueprintGeneratedClass::GetWidgetTreeArchetype()
	// so the value survives the next compile_blueprint pass.
	//
	// Why the archetype write matters: Wbp->WidgetTree is the source-of-truth
	// authoring tree; UWidgetBlueprintGeneratedClass::WidgetTree (accessible via
	// GetWidgetTreeArchetype()) is the CDO-time archetype copy that the engine
	// uses to instantiate widgets at runtime. CompileBlueprint refreshes the
	// archetype from the authoring tree, but Python `set_editor_property` against
	// the AUTHORING tree alone doesn't always propagate — the symptom from the
	// session debug was "Python set works once, compile_blueprint reverts it".
	// Writing to BOTH trees here keeps the value sticky across recompiles.

	static FMonolithActionResult HandleSetActionBarButtonClass(const TSharedPtr<FJsonObject>& Params)
	{
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		FString WidgetName, ButtonClassPath;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("widget_name"), WidgetName)
			|| !Params->TryGetStringField(TEXT("button_class"), ButtonClassPath))
		{
			return FMonolithActionResult::Error(TEXT("widget_name and button_class required"));
		}

		// Resolve the button class — same logic as create_bound_action_bar (line ~230).
		// Accept full /Script/Module.ClassName paths, _C blueprint class paths,
		// and bare class names via FindFirstObject.
		UClass* ResolvedButtonClass = LoadClass<UObject>(nullptr, *ButtonClassPath);
		if (!ResolvedButtonClass)
		{
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ButtonClassPath))
			{
				ResolvedButtonClass = BP->GeneratedClass;
			}
		}
		if (!ResolvedButtonClass)
		{
			ResolvedButtonClass = FindFirstObject<UClass>(*ButtonClassPath, EFindFirstObjectOptions::NativeFirst);
		}
		if (!ResolvedButtonClass)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to resolve button_class '%s'. Use a /Script/Module.ClassName path, "
					 "a /Game/...ClassName_C blueprint class path, or a registered native class name."),
				*ButtonClassPath));
		}
		if (!ResolvedButtonClass->IsChildOf(UCommonButtonBase::StaticClass()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("button_class '%s' is not a UCommonButtonBase subclass — UCommonBoundActionBar::ActionButtonClass "
					 "rejects non-CommonButtonBase classes at validation."),
				*ButtonClassPath));
		}

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		UCommonBoundActionBar* Bar = Cast<UCommonBoundActionBar>(Target);
		if (!Bar)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Widget '%s' is not a UCommonBoundActionBar (actual class: %s)"),
				*WidgetName, *Target->GetClass()->GetName()));
		}

		// ----- Reflection write #1: authoring tree -----------------------------
		// FClassProperty pattern verbatim from Phase 1 Bug #4
		// (MonolithCommonUIInputActions.cpp:271-282). ActionButtonClass is a
		// private UPROPERTY (CommonBoundActionBar.h:67-68), direct assignment
		// fails C2248.
		FClassProperty* ActionButtonClassProp = FindFProperty<FClassProperty>(
			UCommonBoundActionBar::StaticClass(), TEXT("ActionButtonClass"));
		if (!ActionButtonClassProp)
		{
			return FMonolithActionResult::Error(TEXT(
				"Failed to resolve UCommonBoundActionBar::ActionButtonClass FClassProperty via reflection — "
				"engine API drift?"), -32603);
		}

		ActionButtonClassProp->SetObjectPropertyValue(
			ActionButtonClassProp->ContainerPtrToValuePtr<void>(Bar),
			ResolvedButtonClass);

		// ----- Reflection write #2: archetype tree (compile-survival) ----------
		// Find the same widget by name in the WidgetBlueprintGeneratedClass's
		// archetype tree and write through there too. WidgetTree is private; the
		// public accessor is GetWidgetTreeArchetype() — WidgetBlueprintGeneratedClass.h:159.
		bool bArchetypeWritten = false;
		if (UWidgetBlueprintGeneratedClass* WBGC = Cast<UWidgetBlueprintGeneratedClass>(Wbp->GeneratedClass))
		{
			if (UWidgetTree* ArchetypeTree = WBGC->GetWidgetTreeArchetype())
			{
				if (UWidget* ArchetypeWidget = ArchetypeTree->FindWidget(FName(*WidgetName)))
				{
					if (UCommonBoundActionBar* ArchetypeBar = Cast<UCommonBoundActionBar>(ArchetypeWidget))
					{
						ActionButtonClassProp->SetObjectPropertyValue(
							ActionButtonClassProp->ContainerPtrToValuePtr<void>(ArchetypeBar),
							ResolvedButtonClass);
						bArchetypeWritten = true;
					}
				}
			}
		}

		Bar->Modify();
		Wbp->Modify();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("action_button_class"), ResolvedButtonClass->GetPathName());
		Result->SetBoolField(TEXT("authoring_tree_written"), true);
		Result->SetBoolField(TEXT("archetype_tree_written"), bArchetypeWritten);
		if (!bArchetypeWritten)
		{
			Result->SetStringField(TEXT("archetype_note"),
				TEXT("Could not write to GetWidgetTreeArchetype() — value may revert after a subsequent "
					 "compile_blueprint pass. The compile triggered here refreshes the archetype from the "
					 "authoring tree so this is typically benign, but a manual recompile via blueprint_query "
					 "may reset the value. Worth investigating if the value does not persist."));
		}
		return FMonolithActionResult::Success(Result);
	}

	// ----- convert_border_to_common --------------------------------------------
	//
	// Replace a UBorder with a UCommonBorder, preserving the variable identity,
	// parent slot, AND the single content child. Unlike the UButton ->
	// UCommonButtonBase case (which crosses from UContentWidget to a
	// UCommonUserWidget with an internal tree), both UBorder and UCommonBorder are
	// UContentWidget subclasses, so the content child transfers cleanly via
	// SetContent. UCommonBorder is a concrete (non-abstract) UCLASS, so we
	// construct it directly — no on-demand subclass creation needed.

	static FMonolithActionResult HandleConvertBorderToCommon(const TSharedPtr<FJsonObject>& Params)
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

		UBorder* OldBorder = Cast<UBorder>(Target);
		if (!OldBorder)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' is not a UBorder"), *WidgetName));
		// A UCommonBorder is itself a UBorder; converting it would be a no-op.
		if (OldBorder->IsA<UCommonBorder>())
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' is already a UCommonBorder"), *WidgetName));

		UPanelSlot* ParentSlot = OldBorder->Slot;
		UPanelWidget* Parent = ParentSlot ? ParentSlot->Parent : nullptr;
		const bool bIsRoot = (Wbp->WidgetTree && Wbp->WidgetTree->RootWidget == OldBorder);
		if (!Parent && !bIsRoot)
			return FMonolithActionResult::Error(TEXT("Cannot convert a border with no parent that is also not the tree root"));

		// Detach the existing content child so it survives the swap.
		UWidget* Content = OldBorder->GetContent();
		if (Content)
		{
			OldBorder->Modify();
			OldBorder->ClearChildren();
		}

		const FName BorderName = OldBorder->GetFName();
		const FName TempName = MakeUniqueObjectName(
			Wbp->WidgetTree, UCommonBorder::StaticClass(),
			FName(*FString::Printf(TEXT("%s_CommonReplacement"), *BorderName.ToString())));

		UCommonBorder* NewBorder = Wbp->WidgetTree->ConstructWidget<UCommonBorder>(UCommonBorder::StaticClass(), TempName);
		if (!NewBorder)
			return FMonolithActionResult::Error(TEXT("ConstructWidget<UCommonBorder> returned null"));

		// Splice the new border into the old border's place (parent slot OR tree root).
		OldBorder->Modify();
		if (bIsRoot)
		{
			OldBorder->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			NewBorder->Rename(*BorderName.ToString(), Wbp->WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty);
			Wbp->WidgetTree->RootWidget = NewBorder;
		}
		else
		{
			Parent->RemoveChild(OldBorder);
			OldBorder->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			NewBorder->Rename(*BorderName.ToString(), Wbp->WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty);
			Parent->AddChild(NewBorder);
		}
		MonolithUI::RegisterCreatedWidget(Wbp, NewBorder);

		// Reattach the preserved content child to the new border.
		if (Content)
		{
			NewBorder->SetContent(Content);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		MonolithUI::ReconcileWidgetVariableGuids(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), BorderName.ToString());
		Result->SetStringField(TEXT("new_class"), UCommonBorder::StaticClass()->GetName());
		Result->SetBoolField(TEXT("was_root"), bIsRoot);
		Result->SetBoolField(TEXT("had_content"), Content != nullptr);
		if (Content)
		{
			Result->SetStringField(TEXT("preserved_content"), Content->GetName());
		}
		return FMonolithActionResult::Success(Result);
	}

	// ----- reparent_widget_root ------------------------------------------------
	//
	// Replace a WBP's root widget with a new widget of an arbitrary class, moving
	// the old root's children onto the new root. The new class is resolved by
	// STRING (FindFirstObject / LoadClass) so this stays engine-generic — no
	// hardcoded sibling/marketplace widget types. The new class must derive from
	// UPanelWidget (it must be able to host children) and be concrete.

	static FMonolithActionResult HandleReparentWidgetRoot(const TSharedPtr<FJsonObject>& Params)
	{
		FString NewClassName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("new_class"), NewClassName) || NewClassName.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path and new_class required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		// Resolve the new root class by string — never a hardcoded type.
		UClass* NewClass = FindFirstObject<UClass>(*NewClassName, EFindFirstObjectOptions::NativeFirst);
		if (!NewClass && !NewClassName.StartsWith(TEXT("U")))
			NewClass = FindFirstObject<UClass>(*(TEXT("U") + NewClassName), EFindFirstObjectOptions::NativeFirst);
		if (!NewClass)
			NewClass = LoadClass<UObject>(nullptr, *NewClassName);
		if (!NewClass)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("new_class '%s' could not be resolved. Use /Script/Module.ClassName, a /Game/... _C path, or a loaded class name."),
				*NewClassName));

		if (!NewClass->IsChildOf(UPanelWidget::StaticClass()))
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("new_class '%s' is not a UPanelWidget subclass — a root that hosts children must be a panel widget"), *NewClassName));
		if (NewClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("new_class '%s' is abstract/deprecated — cannot construct"), *NewClassName));

		UWidget* OldRoot = Wbp->WidgetTree->RootWidget;
		if (!OldRoot)
			return FMonolithActionResult::Error(TEXT("WBP has no root widget to reparent"));

		// Gather the old root's direct children (if it was a panel) to migrate.
		TArray<UWidget*> Children;
		if (UPanelWidget* OldPanel = Cast<UPanelWidget>(OldRoot))
		{
			OldPanel->Modify();
			for (int32 i = 0; i < OldPanel->GetChildrenCount(); ++i)
			{
				if (UWidget* Child = OldPanel->GetChildAt(i))
				{
					Children.Add(Child);
				}
			}
			OldPanel->ClearChildren();
		}

		const FName OldRootName = OldRoot->GetFName();
		const FName TempName = MakeUniqueObjectName(
			Wbp->WidgetTree, NewClass,
			FName(*FString::Printf(TEXT("%s_ReparentedRoot"), *OldRootName.ToString())));

		UPanelWidget* NewRoot = Wbp->WidgetTree->ConstructWidget<UPanelWidget>(NewClass, TempName);
		if (!NewRoot)
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("ConstructWidget returned null for new_class '%s'"), *NewClassName));

		OldRoot->Modify();
		OldRoot->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
		NewRoot->Rename(*OldRootName.ToString(), Wbp->WidgetTree, REN_DontCreateRedirectors | REN_DoNotDirty);
		Wbp->WidgetTree->RootWidget = NewRoot;
		MonolithUI::RegisterCreatedWidget(Wbp, NewRoot);

		// Migrate the preserved children onto the new root.
		int32 Migrated = 0;
		for (UWidget* Child : Children)
		{
			if (Child && NewRoot->AddChild(Child))
			{
				++Migrated;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		MonolithUI::ReconcileWidgetVariableGuids(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("root_widget"), OldRootName.ToString());
		Result->SetStringField(TEXT("new_class"), NewClass->GetName());
		Result->SetNumberField(TEXT("children_migrated"), Migrated);
		Result->SetNumberField(TEXT("children_total"), Children.Num());
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

		// Phase 2 Item #10 (2026-05-16 UI gap audit): apply_token_binding.
		// MVP-STUB — Tokenforge probe + param validation are FULL; BP-graph
		// node-write into NativeConstruct is deferred (issue #2-10b). Returns
		// -32011 ErrTokenforgeRuntimeUnavailable when the plugin is absent.
		Registry.RegisterAction(
			TEXT("ui"), TEXT("apply_token_binding"),
			TEXT("Bind a widget property to a UI design token sourced from TokenforgeRuntime. "
				 "Returns -32011 with {dep_name, widget_type, alternative, category} when Tokenforge is not enabled "
				 "(mirrors the -32010 EffectSurface contract). Current implementation level: MVP-STUB — "
				 "param validation and Tokenforge probe FULL, BP-graph node-write into NativeConstruct deferred. "
				 "Successful response carries status='stub' so callers can branch on partial implementation."),
			FMonolithActionHandler::CreateStatic(&HandleApplyTokenBinding),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget FName"))
				.Required(TEXT("target_property"), TEXT("string"), TEXT("UPROPERTY name on the widget to drive from the token"))
				.Required(TEXT("token_key"), TEXT("string"), TEXT("Tokenforge token identifier (e.g. 'color.surface.default')"))
				.Build(),
			Cat);

		// Phase 2 Item #12 (2026-05-16 UI gap audit): convert_textblock_to_common.
		// Mirrors convert_button_to_common's reconciliation pattern. Variable
		// identity preserved (FName + bIsVariable). Reattaches to the same parent
		// slot. Captured state: Text, Font, ColorAndOpacity, ShadowColorAndOpacity,
		// ShadowOffset. Style left at engine default — caller chains apply_style_to_widget.
		Registry.RegisterAction(
			TEXT("ui"), TEXT("convert_textblock_to_common"),
			TEXT("Replace a UTextBlock in a WBP with a UCommonTextBlock, preserving the variable identity, "
				 "parent slot, and authored text/font/colour/shadow state. Style left at engine default — "
				 "chain apply_style_to_widget with a UCommonTextStyle reference to complete the rethemed migration."),
			FMonolithActionHandler::CreateStatic(&HandleConvertTextBlockToCommon),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the UTextBlock to convert"))
				.Build(),
			Cat);

		// Phase 2 Item #13 (2026-05-16 UI gap audit): set_action_bar_button_class.
		// FClassProperty reflection write on an existing UCommonBoundActionBar
		// in a WBP. Mirrors Phase 1 Bug #4 (MonolithCommonUIInputActions.cpp:265-282)
		// but additionally writes through UWidgetBlueprintGeneratedClass::
		// GetWidgetTreeArchetype() so the value survives subsequent compile_blueprint
		// passes (the symptom the session debug was hitting via Python
		// set_editor_property).
		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_action_bar_button_class"),
			TEXT("Set UCommonBoundActionBar::ActionButtonClass on an existing bar widget in a WBP. "
				 "Writes through BOTH the authoring tree (Wbp->WidgetTree) AND the generated class's "
				 "archetype tree (UWidgetBlueprintGeneratedClass::GetWidgetTreeArchetype()) so the value "
				 "survives recompile passes. button_class must be a UCommonButtonBase subclass — _C path, "
				 "/Script/Module.ClassName path, or registered native class name."),
			FMonolithActionHandler::CreateStatic(&HandleSetActionBarButtonClass),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the UCommonBoundActionBar"))
				.Required(TEXT("button_class"), TEXT("string"), TEXT("UCommonButtonBase subclass path (e.g. /Game/UI/BP_MyButton.BP_MyButton_C)"))
				.Build(),
			Cat);

		// Phase 3 Item #1 (2026-05-23 UI/BP gap closure): convert_border_to_common.
		// UBorder -> UCommonBorder (both UContentWidget) preserving variable
		// identity, parent slot / tree-root position, AND the single content child.
		Registry.RegisterAction(
			TEXT("ui"), TEXT("convert_border_to_common"),
			TEXT("Replace a UBorder in a WBP with a UCommonBorder, preserving the variable identity, parent slot "
				 "(or tree-root position), and the single content child. UCommonBorder is concrete so no target_class "
				 "is needed. Chain apply_style_to_widget with a UCommonBorderStyle to finish the rethemed migration."),
			FMonolithActionHandler::CreateStatic(&HandleConvertBorderToCommon),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the UBorder to convert"))
				.Build(),
			Cat);

		// Phase 3 Item #1 (2026-05-23 UI/BP gap closure): reparent_widget_root.
		// General-form root swap — new_class resolved by STRING (engine-generic,
		// no hardcoded sibling/marketplace types). New root must be a concrete
		// UPanelWidget subclass; the old root's children migrate onto it.
		Registry.RegisterAction(
			TEXT("ui"), TEXT("reparent_widget_root"),
			TEXT("Replace a WBP's root widget with a new UPanelWidget-derived class (resolved by string: "
				 "/Script/Module.ClassName, a /Game/... _C path, or a loaded class name), migrating the old root's "
				 "children onto the new root. new_class must be a concrete UPanelWidget subclass."),
			FMonolithActionHandler::CreateStatic(&HandleReparentWidgetRoot),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Required(TEXT("new_class"), TEXT("string"), TEXT("New root class (UPanelWidget subclass) resolved by string"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
