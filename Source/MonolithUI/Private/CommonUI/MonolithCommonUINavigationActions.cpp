// Category D: Navigation, Focus, Rules — 5 actions
// 3.D.1 set_widget_navigation
// 3.D.2 set_initial_focus_target
// 3.D.3 force_focus [RUNTIME]
// 3.D.4 get_focus_path [RUNTIME]
// 3.D.5 request_refresh_focus [RUNTIME]
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "CommonActivatableWidget.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Types/NavigationMetaData.h"
#include "Blueprint/WidgetNavigation.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "Blueprint/UserWidget.h"

namespace MonolithCommonUINavigation
{
	static bool ParseDirection(const FString& S, EUINavigation& Out)
	{
		if (S.Equals(TEXT("Up"), ESearchCase::IgnoreCase))       { Out = EUINavigation::Up; return true; }
		if (S.Equals(TEXT("Down"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Down; return true; }
		if (S.Equals(TEXT("Left"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Left; return true; }
		if (S.Equals(TEXT("Right"), ESearchCase::IgnoreCase))    { Out = EUINavigation::Right; return true; }
		if (S.Equals(TEXT("Next"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Next; return true; }
		if (S.Equals(TEXT("Previous"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Previous; return true; }
		return false;
	}

	static bool ParseRule(const FString& S, EUINavigationRule& Out)
	{
		if (S.Equals(TEXT("Escape"), ESearchCase::IgnoreCase))   { Out = EUINavigationRule::Escape; return true; }
		if (S.Equals(TEXT("Stop"), ESearchCase::IgnoreCase))     { Out = EUINavigationRule::Stop; return true; }
		if (S.Equals(TEXT("Wrap"), ESearchCase::IgnoreCase))     { Out = EUINavigationRule::Wrap; return true; }
		if (S.Equals(TEXT("Explicit"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Explicit; return true; }
		if (S.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))   { Out = EUINavigationRule::Custom; return true; }
		if (S.Equals(TEXT("CustomBoundary"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::CustomBoundary; return true; }
		return false;
	}

	// Forward declarations — the rule/direction stringifiers are defined further
	// down (shared with audit_focus_chain) but are referenced by the bulk/dump
	// handlers above their definitions.
	static FString NavRuleToString(EUINavigationRule Rule);
	static FString NavDirectionToString(EUINavigation Dir);

	// ----- 3.D.1 set_widget_navigation -----------------------------------------

	static FMonolithActionResult HandleSetWidgetNavigation(const TSharedPtr<FJsonObject>& Params)
	{
		FString WidgetName, DirStr, RuleStr;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("widget_name"), WidgetName) ||
			!Params->TryGetStringField(TEXT("direction"), DirStr) ||
			!Params->TryGetStringField(TEXT("rule"), RuleStr))
			return FMonolithActionResult::Error(TEXT("wbp_path, widget_name, direction, rule required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		EUINavigation Dir;
		if (!ParseDirection(DirStr, Dir))
			return FMonolithActionResult::Error(TEXT("direction must be Up/Down/Left/Right/Next/Previous"));
		EUINavigationRule Rule;
		if (!ParseRule(RuleStr, Rule))
			return FMonolithActionResult::Error(TEXT("rule must be Escape/Stop/Wrap/Explicit/Custom/CustomBoundary"));

		UWidgetBlueprint* Wbp = nullptr;
		UWidget* Target = nullptr;
		FMonolithActionResult Loaded = MonolithCommonUI::LoadWidgetForMutation(WbpPath, FName(*WidgetName), Wbp, Target);
		if (!Loaded.bSuccess) return Loaded;

		if (Rule == EUINavigationRule::Explicit)
		{
			FString ExplicitTargetName;
			if (!Params->TryGetStringField(TEXT("explicit_target"), ExplicitTargetName))
				return FMonolithActionResult::Error(TEXT("rule=Explicit requires explicit_target widget name"));

			UWidget* ExplicitTarget = nullptr;
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (!ExplicitTarget && W && W->GetFName() == FName(*ExplicitTargetName))
					ExplicitTarget = W;
			});
			if (!ExplicitTarget)
				return FMonolithActionResult::Error(FString::Printf(TEXT("explicit_target '%s' not found"), *ExplicitTargetName));

			Target->SetNavigationRuleExplicit(Dir, ExplicitTarget);
		}
		else
		{
			Target->SetNavigationRuleBase(Dir, Rule);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("direction"), DirStr);
		Result->SetStringField(TEXT("rule"), RuleStr);
		return FMonolithActionResult::Success(Result);
	}

	// ----- set_widget_navigation_bulk ------------------------------------------
	//
	// Apply N navigation-rule writes across a WBP, then compile ONCE. The
	// single-write set_widget_navigation compiles per call; for callers wiring an
	// entire screen's nav graph that is N redundant compiles. This action mirrors
	// the same widget/rule resolution but defers MarkBlueprintAsStructurallyModified
	// + CompileBlueprint to a single pass after every write succeeds.
	//
	// Each entry: { widget_name, direction, rule, explicit_target? }. Per-entry
	// failures are collected (not fatal) so a partial batch still applies the
	// valid writes and reports which entries failed and why.

	static FMonolithActionResult HandleSetWidgetNavigationBulk(const TSharedPtr<FJsonObject>& Params)
	{
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
			return FMonolithActionResult::Error(TEXT("entries array required: [{widget_name, direction, rule, explicit_target?}]"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		// Cache widget lookups so a multi-entry batch resolves each widget once.
		TMap<FName, UWidget*> WidgetByName;
		Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (W)
			{
				WidgetByName.Add(W->GetFName(), W);
			}
		});

		auto ResolveWidget = [&](const FString& Name) -> UWidget*
		{
			UWidget** Found = WidgetByName.Find(FName(*Name));
			return Found ? *Found : nullptr;
		};

		int32 Written = 0;
		TArray<TSharedPtr<FJsonValue>> Failed;

		auto FailEntry = [&](int32 Index, const FString& Reason)
		{
			TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetNumberField(TEXT("index"), Index);
			F->SetStringField(TEXT("reason"), Reason);
			Failed.Add(MakeShared<FJsonValueObject>(F));
		};

		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!(*Entries)[Index]->TryGetObject(EntryObj) || !EntryObj || !(*EntryObj).IsValid())
			{
				FailEntry(Index, TEXT("entry is not an object"));
				continue;
			}

			FString WidgetName, DirStr, RuleStr;
			if (!(*EntryObj)->TryGetStringField(TEXT("widget_name"), WidgetName) ||
				!(*EntryObj)->TryGetStringField(TEXT("direction"), DirStr) ||
				!(*EntryObj)->TryGetStringField(TEXT("rule"), RuleStr))
			{
				FailEntry(Index, TEXT("widget_name, direction, rule required"));
				continue;
			}

			EUINavigation Dir;
			if (!ParseDirection(DirStr, Dir))
			{
				FailEntry(Index, TEXT("direction must be Up/Down/Left/Right/Next/Previous"));
				continue;
			}
			EUINavigationRule Rule;
			if (!ParseRule(RuleStr, Rule))
			{
				FailEntry(Index, TEXT("rule must be Escape/Stop/Wrap/Explicit/Custom/CustomBoundary"));
				continue;
			}

			UWidget* Target = ResolveWidget(WidgetName);
			if (!Target)
			{
				FailEntry(Index, FString::Printf(TEXT("widget '%s' not found"), *WidgetName));
				continue;
			}

			if (Rule == EUINavigationRule::Explicit)
			{
				FString ExplicitTargetName;
				if (!(*EntryObj)->TryGetStringField(TEXT("explicit_target"), ExplicitTargetName))
				{
					FailEntry(Index, TEXT("rule=Explicit requires explicit_target"));
					continue;
				}
				UWidget* ExplicitTarget = ResolveWidget(ExplicitTargetName);
				if (!ExplicitTarget)
				{
					FailEntry(Index, FString::Printf(TEXT("explicit_target '%s' not found"), *ExplicitTargetName));
					continue;
				}
				Target->SetNavigationRuleExplicit(Dir, ExplicitTarget);
			}
			else
			{
				Target->SetNavigationRuleBase(Dir, Rule);
			}
			++Written;
		}

		// Single deferred compile after all writes — the whole point of the bulk path.
		// When save is requested, route through CompileAndSaveWidgetBlueprint which
		// performs the mark/compile/save in one pass (no double-compile).
		bool bCompiledOnce = false;
		if (Written > 0)
		{
			bool bSave = false;
			Params->TryGetBoolField(TEXT("save"), bSave);
			if (bSave)
			{
				MonolithCommonUI::CompileAndSaveWidgetBlueprint(Wbp);
			}
			else
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
				FKismetEditorUtilities::CompileBlueprint(Wbp);
				Wbp->GetOutermost()->MarkPackageDirty();
			}
			bCompiledOnce = true;
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetNumberField(TEXT("written"), Written);
		Result->SetArrayField(TEXT("failed"), Failed);
		Result->SetBoolField(TEXT("compiled_once"), bCompiledOnce);
		return FMonolithActionResult::Success(Result);
	}

	// ----- dump_widget_navigation ----------------------------------------------
	//
	// Read-only dump of UWidget::Navigation per-direction rules. audit_focus_chain
	// only surfaces Explicit edges (its graph is built from explicit targets);
	// Wrap/Stop/Escape rules are invisible to it. This action reports the raw rule
	// for every authored direction so callers can see the full nav configuration.
	// Returns one entry per (widget, direction) that has an authored rule.

	static FMonolithActionResult HandleDumpWidgetNavigation(const TSharedPtr<FJsonObject>& Params)
	{
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		// Optional filter: a single widget by name.
		FString FilterName;
		const bool bHasFilter = Params->TryGetStringField(TEXT("widget_name"), FilterName);

		TArray<TSharedPtr<FJsonValue>> Out;

		// Emit one JSON entry per authored direction of a UWidgetNavigation.
		auto EmitDir = [&](const FName& WidgetName, const FWidgetNavigationData& Data, EUINavigation Dir)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("widget_name"), WidgetName.ToString());
			Entry->SetStringField(TEXT("direction"), NavDirectionToString(Dir));
			Entry->SetStringField(TEXT("rule"), NavRuleToString(Data.Rule));
			if (Data.Rule == EUINavigationRule::Explicit)
			{
				// Widget pointer is only resolved at PIE; fall back to the
				// editor-stored WidgetToFocus FName (FWidgetNavigationData.h:33).
				FName TargetName;
				if (Data.Widget.IsValid())
				{
					TargetName = Data.Widget->GetFName();
				}
				else if (!Data.WidgetToFocus.IsNone())
				{
					TargetName = Data.WidgetToFocus;
				}
				if (!TargetName.IsNone())
				{
					Entry->SetStringField(TEXT("target"), TargetName.ToString());
				}
			}
			Out.Add(MakeShared<FJsonValueObject>(Entry));
		};

		Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (!W) return;
			if (bHasFilter && W->GetFName() != FName(*FilterName)) return;

			UWidgetNavigation* Nav = W->Navigation;
			if (!Nav) return;

			const FName WName = W->GetFName();
			EmitDir(WName, Nav->Up,       EUINavigation::Up);
			EmitDir(WName, Nav->Down,     EUINavigation::Down);
			EmitDir(WName, Nav->Left,     EUINavigation::Left);
			EmitDir(WName, Nav->Right,    EUINavigation::Right);
			EmitDir(WName, Nav->Next,     EUINavigation::Next);
			EmitDir(WName, Nav->Previous, EUINavigation::Previous);
		});

		// The action contract returns an array of {widget_name, direction, rule, target?}.
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetArrayField(TEXT("navigation"), Out);
		Result->SetNumberField(TEXT("count"), Out.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.2 set_initial_focus_target --------------------------------------
	// UCommonActivatableWidget uses GetDesiredFocusTarget() which can be overridden via
	// NativeGetDesiredFocusTarget / BP_GetDesiredFocusTarget. In CDO terms there's no direct
	// property — the canonical pattern is to implement BP_GetDesiredFocusTarget in the WBP.
	// For a programmatic setter, we stamp a property we name "InitialFocusTarget" on the CDO
	// if the widget class exposes one; otherwise fall through to a metadata sentinel the
	// WBP author can query in their GetDesiredFocusTarget override. For most Leviathan use,
	// the simpler mechanism is: set a named UWidget* reference property. We probe for a
	// known-name convention (`DesiredFocusTarget` / `InitialFocusTarget`) on the WBP class.

	static FMonolithActionResult HandleSetInitialFocusTarget(const TSharedPtr<FJsonObject>& Params)
	{
		FString TargetWidgetName;
		if (!Params.IsValid() ||
			!Params->TryGetStringField(TEXT("target_widget"), TargetWidgetName))
			return FMonolithActionResult::Error(TEXT("wbp_path and target_widget required"));
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->GeneratedClass)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		if (!Wbp->GeneratedClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
			return FMonolithActionResult::Error(TEXT("WBP is not a UCommonActivatableWidget — focus target requires activatable parent"));

		// Ensure the target widget exists
		bool bTargetFound = false;
		if (Wbp->WidgetTree)
		{
			Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
			{
				if (!bTargetFound && W && W->GetFName() == FName(*TargetWidgetName))
					bTargetFound = true;
			});
		}
		if (!bTargetFound)
			return FMonolithActionResult::Error(FString::Printf(TEXT("target_widget '%s' not in WBP tree"), *TargetWidgetName));

		// Store target as FName via a dedicated UPROPERTY if present, else record as metadata.
		FName PropNames[] = { TEXT("DesiredFocusTargetName"), TEXT("InitialFocusTargetName") };
		UCommonActivatableWidget* CDO = Cast<UCommonActivatableWidget>(Wbp->GeneratedClass->GetDefaultObject());
		bool bStored = false;
		for (const FName& PN : PropNames)
		{
			if (FNameProperty* P = FindFProperty<FNameProperty>(Wbp->GeneratedClass, PN))
			{
				P->SetPropertyValue_InContainer(CDO, FName(*TargetWidgetName));
				bStored = true;
				break;
			}
		}

		if (!bStored)
		{
			return FMonolithActionResult::Error(
				TEXT("WBP has no DesiredFocusTargetName / InitialFocusTargetName FName UPROPERTY. Add one and override NativeGetDesiredFocusTarget to return WidgetTree->FindWidget(DesiredFocusTargetName)."));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);
		Wbp->GetOutermost()->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("target_widget"), TargetWidgetName);
		Result->SetBoolField(TEXT("stored_to_cdo"), bStored);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Runtime widget lookup by name in PIE viewport widgets ----------------

	static UWidget* FindWidgetInPIE(const FName& WidgetName)
	{
		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		if (!PIE) return nullptr;

		for (TObjectIterator<UWidget> It; It; ++It)
		{
			UWidget* W = *It;
			if (!W || W->GetWorld() != PIE) continue;
			if (W->GetFName() == WidgetName) return W;
		}
		return nullptr;
	}

	// ----- 3.D.3 force_focus [RUNTIME] -----------------------------------------

	static FMonolithActionResult HandleForceFocus(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("widget_name required"));

		UWidget* Target = FindWidgetInPIE(FName(*WidgetName));
		if (!Target)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Widget '%s' not found in PIE"), *WidgetName));

		Target->SetFocus();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetStringField(TEXT("widget_class"), Target->GetClass()->GetName());
		Result->SetBoolField(TEXT("has_user_focus"), Target->HasUserFocus(0));
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.4 get_focus_path [RUNTIME] --------------------------------------

	static FMonolithActionResult HandleGetFocusPath(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		if (!FSlateApplication::IsInitialized())
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));

		FSlateApplication& App = FSlateApplication::Get();
		TSharedPtr<SWidget> FocusedSlate = App.GetUserFocusedWidget(0);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!FocusedSlate.IsValid())
		{
			Result->SetBoolField(TEXT("has_focus"), false);
			return FMonolithActionResult::Success(Result);
		}

		Result->SetBoolField(TEXT("has_focus"), true);
		Result->SetStringField(TEXT("focused_slate_type"), FocusedSlate->GetTypeAsString());

		// Walk up the Slate tree describing each widget
		TArray<TSharedPtr<FJsonValue>> Path;
		TSharedPtr<SWidget> Cur = FocusedSlate;
		while (Cur.IsValid())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("type"), Cur->GetTypeAsString());
			Entry->SetStringField(TEXT("tag"), Cur->GetTag().ToString());
			Path.Add(MakeShared<FJsonValueObject>(Entry));
			Cur = Cur->GetParentWidget();
		}
		Result->SetArrayField(TEXT("slate_path_leaf_to_root"), Path);

		return FMonolithActionResult::Success(Result);
	}

	// ----- Phase 2 Item #9 — audit_focus_chain ---------------------------------
	//
	// Static analysis of a WBP's focus / navigation chain. Walks the WidgetTree,
	// reads each UWidget's Navigation UPROPERTY (TObjectPtr<UWidgetNavigation>
	// at Widget.h:466), and builds an adjacency map per direction. Produces:
	//   * desired_focus_target — value of DesiredFocusTargetName / InitialFocusTargetName
	//                            UPROPERTY on the WBP CDO if present
	//   * unreachable[]        — focusable widgets that no inbound nav edge reaches
	//                            from the desired focus target (BFS-by-direction)
	//   * dead_ends[]          — focusable widgets with no outbound nav edges
	//                            (no Escape/Stop is unusual but not a defect on its own;
	//                            we report Explicit-direction-with-null-target instead)
	//   * cycles[]             — explicit-nav cycles detected by DFS (rare, but breaks
	//                            screenreader/gamepad nav loops)
	//
	// The action is read-only: no Modify, no Compile, no SavePackage. Returns a
	// structured report so the LLM can decide whether to dispatch set_widget_navigation
	// / set_initial_focus_target fixes.

	struct FFocusAuditWidget
	{
		FName Name;
		bool bIsFocusable = false;
		bool bIsActivatable = false;
		// Explicit-nav outbound edges (direction -> target widget name).
		// Only populated for EUINavigationRule::Explicit entries.
		TMap<EUINavigation, FName> ExplicitEdges;
		// All rules per direction (Escape/Stop/Wrap/Explicit/Custom/CustomBoundary).
		// Used to detect "no outbound nav at all" dead-ends.
		TMap<EUINavigation, EUINavigationRule> Rules;
	};

	static FString NavRuleToString(EUINavigationRule Rule)
	{
		switch (Rule)
		{
			case EUINavigationRule::Escape:         return TEXT("Escape");
			case EUINavigationRule::Stop:           return TEXT("Stop");
			case EUINavigationRule::Wrap:           return TEXT("Wrap");
			case EUINavigationRule::Explicit:       return TEXT("Explicit");
			case EUINavigationRule::Custom:         return TEXT("Custom");
			case EUINavigationRule::CustomBoundary: return TEXT("CustomBoundary");
			default:                                return TEXT("Unknown");
		}
	}

	static FString NavDirectionToString(EUINavigation Dir)
	{
		switch (Dir)
		{
			case EUINavigation::Up:       return TEXT("Up");
			case EUINavigation::Down:     return TEXT("Down");
			case EUINavigation::Left:     return TEXT("Left");
			case EUINavigation::Right:    return TEXT("Right");
			case EUINavigation::Next:     return TEXT("Next");
			case EUINavigation::Previous: return TEXT("Previous");
			default:                      return TEXT("Invalid");
		}
	}

	// Iterate the six EUINavigation directions in their canonical order.
	// Centralised here so the BFS/DFS passes below stay short.
	static const TArray<EUINavigation>& AllDirections()
	{
		static const TArray<EUINavigation> Dirs = {
			EUINavigation::Up, EUINavigation::Down,
			EUINavigation::Left, EUINavigation::Right,
			EUINavigation::Next, EUINavigation::Previous
		};
		return Dirs;
	}

	static FMonolithActionResult HandleAuditFocusChain(const TSharedPtr<FJsonObject>& Params)
	{
		FString WbpPath = MonolithCommonUI::GetWbpPath(Params);
		if (WbpPath.IsEmpty())
			return FMonolithActionResult::Error(TEXT("wbp_path (or asset_path) required"));

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp || !Wbp->WidgetTree)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load WBP '%s'"), *WbpPath));

		// ----- Pass 1: catalog every widget + its outbound nav edges ------------
		//
		// "Focusable" — there is no UWidget-level IsFocusable() in UE 5.7;
		// individual classes expose either an IsFocusable() method (UUserWidget,
		// UComboBoxKey, UComboBoxString) OR a public bool field (UButton::IsFocusable,
		// UCheckBox::IsFocusable, USlider::IsFocusable). To avoid coupling the
		// audit to every focusable subclass we use a conservative inference:
		// any UPanelWidget child that is not a leaf decoration (UTextBlock /
		// UImage / USpacer) AND has an authored Navigation pointer OR is a
		// recognised input/activatable type counts as focusable. False
		// positives (treating a non-focusable widget as focusable) lead to
		// noisier output but never break the audit; false negatives would
		// silently miss real focus-chain defects so we err inclusive.
		TMap<FName, FFocusAuditWidget> Catalog;
		Wbp->WidgetTree->ForEachWidget([&](UWidget* W)
		{
			if (!W) return;

			FFocusAuditWidget Entry;
			Entry.Name = W->GetFName();
			Entry.bIsActivatable = W->IsA<UCommonActivatableWidget>();

			// Inferred focusability — see comment above. Captures the common
			// CommonUI vocabulary (CommonButtonBase, CommonUserWidget, etc.)
			// plus authored-nav-implies-intent-to-focus.
			const bool bIsLeafDecoration =
				W->GetClass()->GetFName() == TEXT("TextBlock")    ||
				W->GetClass()->GetFName() == TEXT("RichTextBlock") ||
				W->GetClass()->GetFName() == TEXT("Image")         ||
				W->GetClass()->GetFName() == TEXT("Spacer")        ||
				W->GetClass()->GetFName() == TEXT("Border");
			const bool bIsUserWidgetDerived = W->IsA<UUserWidget>();
			const bool bHasAuthoredNav = (W->Navigation != nullptr);
			Entry.bIsFocusable =
				!bIsLeafDecoration && (bIsUserWidgetDerived || Entry.bIsActivatable || bHasAuthoredNav);

			// UWidget::Navigation is the per-widget UWidgetNavigation pointer
			// (TObjectPtr<UWidgetNavigation> at Widget.h:466). Null means the
			// widget never had nav rules authored — implicit Escape on all dirs.
			if (UWidgetNavigation* Nav = W->Navigation)
			{
				// FWidgetNavigationData carries {Rule, WidgetToFocus, Widget (TWeakObjectPtr),
				// CustomDelegate}. WidgetNavigation.h:23.
				auto IngestDir = [&](const FWidgetNavigationData& Data, EUINavigation Dir)
				{
					Entry.Rules.Add(Dir, Data.Rule);
					if (Data.Rule == EUINavigationRule::Explicit)
					{
						// Prefer the resolved Widget pointer; fall back to WidgetToFocus FName
						// (the editor stores the name pre-Resolve so the pointer may be null
						// until UWidgetNavigation::Resolve runs at PIE).
						FName TargetName;
						if (Data.Widget.IsValid())
						{
							TargetName = Data.Widget->GetFName();
						}
						else if (!Data.WidgetToFocus.IsNone())
						{
							TargetName = Data.WidgetToFocus;
						}
						if (!TargetName.IsNone())
						{
							Entry.ExplicitEdges.Add(Dir, TargetName);
						}
					}
				};

				IngestDir(Nav->Up,       EUINavigation::Up);
				IngestDir(Nav->Down,     EUINavigation::Down);
				IngestDir(Nav->Left,     EUINavigation::Left);
				IngestDir(Nav->Right,    EUINavigation::Right);
				IngestDir(Nav->Next,     EUINavigation::Next);
				IngestDir(Nav->Previous, EUINavigation::Previous);
			}

			Catalog.Add(Entry.Name, MoveTemp(Entry));
		});

		// ----- Pass 2: resolve desired focus target via CDO ---------------------
		FName DesiredFocusTarget;
		bool bDesiredFocusFromCDO = false;
		if (Wbp->GeneratedClass)
		{
			UObject* CDO = Wbp->GeneratedClass->GetDefaultObject();
			const FName CandidateProps[] = {
				TEXT("DesiredFocusTargetName"), TEXT("InitialFocusTargetName")
			};
			for (const FName& PN : CandidateProps)
			{
				if (FNameProperty* P = FindFProperty<FNameProperty>(Wbp->GeneratedClass, PN))
				{
					const FName Val = P->GetPropertyValue_InContainer(CDO);
					if (!Val.IsNone())
					{
						DesiredFocusTarget = Val;
						bDesiredFocusFromCDO = true;
						break;
					}
				}
			}
		}

		// ----- Pass 3: BFS from desired focus target along explicit edges ------
		//
		// Explicit edges are the only deterministic forward path the audit can
		// trust. Non-Explicit (Escape/Stop/Wrap/Custom) involve runtime focus
		// search across siblings — static analysis cannot resolve that without
		// simulating Slate geometry. We mark non-Explicit widgets as "reachable"
		// only if some Explicit edge lands on them.
		//
		// Widgets unreachable from DesiredFocusTarget are the actionable signal:
		// either the focus chain has a gap, or the WBP wants Escape/Wrap rules
		// that the audit cannot statically verify.

		TSet<FName> Reachable;
		TArray<TSharedPtr<FJsonValue>> Cycles;
		if (!DesiredFocusTarget.IsNone() && Catalog.Contains(DesiredFocusTarget))
		{
			TArray<FName> Queue;
			Queue.Add(DesiredFocusTarget);
			Reachable.Add(DesiredFocusTarget);
			while (Queue.Num() > 0)
			{
				const FName Cur = Queue.Pop(EAllowShrinking::No);
				if (const FFocusAuditWidget* Node = Catalog.Find(Cur))
				{
					for (const TPair<EUINavigation, FName>& Edge : Node->ExplicitEdges)
					{
						if (!Reachable.Contains(Edge.Value))
						{
							Reachable.Add(Edge.Value);
							Queue.Add(Edge.Value);
						}
					}
				}
			}
		}

		// ----- Pass 4: DFS cycle detection on explicit edges --------------------
		//
		// Standard 3-colour DFS: White=unvisited, Gray=on stack, Black=done.
		// Any edge to a Gray node closes a cycle. We report the cycle as the
		// gray-stack tail (entry-to-entry).

		enum class EVisitColor : uint8 { White, Gray, Black };
		TMap<FName, EVisitColor> Color;
		for (const TPair<FName, FFocusAuditWidget>& Kv : Catalog)
		{
			Color.Add(Kv.Key, EVisitColor::White);
		}

		TArray<FName> Stack;
		TFunction<void(const FName&)> Dfs = [&](const FName& N)
		{
			Color[N] = EVisitColor::Gray;
			Stack.Add(N);

			if (const FFocusAuditWidget* Node = Catalog.Find(N))
			{
				for (const TPair<EUINavigation, FName>& Edge : Node->ExplicitEdges)
				{
					if (!Catalog.Contains(Edge.Value)) continue;  // dangling edge — counted elsewhere
					const EVisitColor C = Color.FindRef(Edge.Value);
					if (C == EVisitColor::White)
					{
						Dfs(Edge.Value);
					}
					else if (C == EVisitColor::Gray)
					{
						// Found a back-edge — Stack[idx..end] + Edge.Value forms the cycle.
						const int32 Idx = Stack.IndexOfByKey(Edge.Value);
						if (Idx != INDEX_NONE)
						{
							TArray<TSharedPtr<FJsonValue>> CyclePath;
							for (int32 i = Idx; i < Stack.Num(); ++i)
							{
								CyclePath.Add(MakeShared<FJsonValueString>(Stack[i].ToString()));
							}
							CyclePath.Add(MakeShared<FJsonValueString>(Edge.Value.ToString()));
							TSharedPtr<FJsonObject> CycleObj = MakeShared<FJsonObject>();
							CycleObj->SetArrayField(TEXT("path"), CyclePath);
							CycleObj->SetStringField(TEXT("closed_via_direction"), NavDirectionToString(Edge.Key));
							Cycles.Add(MakeShared<FJsonValueObject>(CycleObj));
						}
					}
				}
			}

			Color[N] = EVisitColor::Black;
			Stack.RemoveAt(Stack.Num() - 1);
		};

		for (const TPair<FName, FFocusAuditWidget>& Kv : Catalog)
		{
			if (Color[Kv.Key] == EVisitColor::White)
			{
				Dfs(Kv.Key);
			}
		}

		// ----- Pass 5: classify per-widget findings -----------------------------

		TArray<TSharedPtr<FJsonValue>> Unreachable;
		TArray<TSharedPtr<FJsonValue>> DeadEnds;
		TArray<TSharedPtr<FJsonValue>> DanglingExplicit;

		for (const TPair<FName, FFocusAuditWidget>& Kv : Catalog)
		{
			const FFocusAuditWidget& W = Kv.Value;

			// Skip non-focusable widgets — they're decorative containers (border,
			// canvas, image) that legitimately have no nav rules.
			if (!W.bIsFocusable) continue;

			if (!DesiredFocusTarget.IsNone() && !Reachable.Contains(W.Name))
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), W.Name.ToString());
				Obj->SetBoolField(TEXT("is_activatable"), W.bIsActivatable);
				Unreachable.Add(MakeShared<FJsonValueObject>(Obj));
			}

			// "Dead end" = focusable widget with NO outbound explicit edges AND
			// no Escape rule that lets focus leave naturally. Empty Rules map
			// (Navigation pointer null) implies engine-default Escape behaviour,
			// which is fine — we only flag when ALL directions are explicit-with-null
			// (the unfixed-explicit-target case) or all Stop (focus trapped).
			if (W.ExplicitEdges.Num() == 0 && W.Rules.Num() > 0)
			{
				bool bAllTrapping = true;
				for (const TPair<EUINavigation, EUINavigationRule>& RP : W.Rules)
				{
					if (RP.Value != EUINavigationRule::Stop)
					{
						bAllTrapping = false;
						break;
					}
				}
				if (bAllTrapping)
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), W.Name.ToString());
					Obj->SetStringField(TEXT("reason"), TEXT("All authored navigation rules are Stop — focus trapped"));
					DeadEnds.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}

			// Dangling explicit target — an Explicit edge that names a widget
			// not in the WBP tree. This is the classic "I renamed a widget and
			// forgot to update the nav reference" defect.
			for (const TPair<EUINavigation, FName>& Edge : W.ExplicitEdges)
			{
				if (!Catalog.Contains(Edge.Value))
				{
					TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), W.Name.ToString());
					Obj->SetStringField(TEXT("direction"), NavDirectionToString(Edge.Key));
					Obj->SetStringField(TEXT("missing_target"), Edge.Value.ToString());
					DanglingExplicit.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
		}

		// ----- Build the response payload --------------------------------------
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetNumberField(TEXT("widget_count"), Catalog.Num());

		if (!DesiredFocusTarget.IsNone())
		{
			Result->SetStringField(TEXT("desired_focus_target"), DesiredFocusTarget.ToString());
			Result->SetBoolField(TEXT("desired_focus_from_cdo"), bDesiredFocusFromCDO);
			Result->SetBoolField(TEXT("desired_focus_target_exists"), Catalog.Contains(DesiredFocusTarget));
		}
		else
		{
			Result->SetBoolField(TEXT("desired_focus_target_exists"), false);
			Result->SetStringField(TEXT("desired_focus_note"),
				TEXT("WBP has no DesiredFocusTargetName / InitialFocusTargetName UPROPERTY — set_initial_focus_target requires the CDO to expose one. BFS reachability not computed."));
		}

		Result->SetArrayField(TEXT("unreachable"),        Unreachable);
		Result->SetArrayField(TEXT("dead_ends"),          DeadEnds);
		Result->SetArrayField(TEXT("cycles"),             Cycles);
		Result->SetArrayField(TEXT("dangling_explicit"),  DanglingExplicit);
		Result->SetNumberField(TEXT("unreachable_count"),       Unreachable.Num());
		Result->SetNumberField(TEXT("dead_end_count"),          DeadEnds.Num());
		Result->SetNumberField(TEXT("cycle_count"),             Cycles.Num());
		Result->SetNumberField(TEXT("dangling_explicit_count"), DanglingExplicit.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ----- 3.D.5 request_refresh_focus [RUNTIME] -------------------------------

	static FMonolithActionResult HandleRequestRefreshFocus(const TSharedPtr<FJsonObject>& Params)
	{
		if (!MonolithCommonUI::GetPIEWorld())
			return FMonolithActionResult::Error(TEXT("requires PIE session"));

		FString WidgetName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("widget_name"), WidgetName))
			return FMonolithActionResult::Error(TEXT("widget_name required (activatable widget FName)"));

		UWorld* PIE = MonolithCommonUI::GetPIEWorld();
		UCommonActivatableWidget* Found = nullptr;
		for (TObjectIterator<UCommonActivatableWidget> It; It; ++It)
		{
			UCommonActivatableWidget* W = *It;
			if (!W || W->GetWorld() != PIE) continue;
			if (W->GetFName() == FName(*WidgetName)) { Found = W; break; }
		}
		if (!Found)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Activatable widget '%s' not found in PIE"), *WidgetName));

		Found->RequestRefreshFocus();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_name"), WidgetName);
		Result->SetBoolField(TEXT("refresh_requested"), true);
		return FMonolithActionResult::Success(Result);
	}

	// ----- Registration --------------------------------------------------------

	void Register(FMonolithToolRegistry& Registry)
	{
		const FString Cat(TEXT("CommonUI"));

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_widget_navigation"),
			TEXT("Set a UWidget's navigation rule for a direction (Up/Down/Left/Right/Next/Previous)"),
			FMonolithActionHandler::CreateStatic(&HandleSetWidgetNavigation),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path"))
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of widget whose nav to set"))
				.Required(TEXT("direction"), TEXT("string"), TEXT("Up|Down|Left|Right|Next|Previous"))
				.Required(TEXT("rule"), TEXT("string"), TEXT("Escape|Stop|Wrap|Explicit|Custom|CustomBoundary"))
				.Optional(TEXT("explicit_target"), TEXT("string"), TEXT("Required when rule=Explicit: target widget name"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_widget_navigation_bulk"),
			TEXT("Apply N navigation-rule writes to a WBP then compile ONCE (vs set_widget_navigation's per-call compile). "
				 "entries: [{widget_name, direction, rule, explicit_target?}]. Per-entry failures are non-fatal and reported "
				 "in 'failed'; valid writes still apply. Returns {written, failed[], compiled_once}."),
			FMonolithActionHandler::CreateStatic(&HandleSetWidgetNavigationBulk),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Required(TEXT("entries"), TEXT("array"), TEXT("[{widget_name, direction(Up|Down|Left|Right|Next|Previous), rule(Escape|Stop|Wrap|Explicit|Custom|CustomBoundary), explicit_target?}]"))
				.Optional(TEXT("save"), TEXT("boolean"), TEXT("Also save the package after the single compile (default false)"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("dump_widget_navigation"),
			TEXT("Read-only dump of UWidget::Navigation per-direction rules (Wrap/Stop/Escape included — these are invisible "
				 "to audit_focus_chain which only graphs Explicit edges). Returns 'navigation': [{widget_name, direction, rule, target?}]. "
				 "Pass widget_name to filter to a single widget. Does not modify or compile the WBP."),
			FMonolithActionHandler::CreateStatic(&HandleDumpWidgetNavigation),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Optional(TEXT("widget_name"), TEXT("string"), TEXT("Filter to a single widget's nav rules"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("set_initial_focus_target"),
			TEXT("Store DesiredFocusTargetName FName UPROPERTY on a UCommonActivatableWidget CDO. WBP must expose this UPROPERTY and override NativeGetDesiredFocusTarget. Satisfying parent classes: UTokenforgeActivatableWidget, UMonolithReduceMotionAwareWidget. If parent doesn't expose the UPROPERTY, use ui::add_widget_variable to add it first."),
			FMonolithActionHandler::CreateStatic(&HandleSetInitialFocusTarget),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("UCommonActivatableWidget blueprint path"))
				.Required(TEXT("target_widget"), TEXT("string"), TEXT("FName of widget to focus when screen activates"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("force_focus"),
			TEXT("[RUNTIME] Call SetFocus on a named widget in the live PIE viewport"),
			FMonolithActionHandler::CreateStatic(&HandleForceFocus),
			FParamSchemaBuilder()
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("FName of UWidget in active UMG tree"))
				.Build(),
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("get_focus_path"),
			TEXT("[RUNTIME] Return the Slate focus chain leaf→root for diagnosing 'why is input eaten' bugs"),
			FMonolithActionHandler::CreateStatic(&HandleGetFocusPath),
			nullptr,
			Cat);

		Registry.RegisterAction(
			TEXT("ui"), TEXT("request_refresh_focus"),
			TEXT("[RUNTIME] Call RequestRefreshFocus on an active UCommonActivatableWidget (after dynamic content swap)"),
			FMonolithActionHandler::CreateStatic(&HandleRequestRefreshFocus),
			FParamSchemaBuilder()
				.Required(TEXT("widget_name"), TEXT("string"), TEXT("FName of active UCommonActivatableWidget"))
				.Build(),
			Cat);

		// Phase 2 Item #9 (2026-05-16 UI gap audit): audit_focus_chain.
		// Static analysis of the WBP's nav graph — no PIE / runtime needed.
		// Read-only: never modifies the asset, never compiles. Returns
		// {desired_focus_target, unreachable[], dead_ends[], cycles[],
		//  dangling_explicit[]}. BFS reachability runs only when DesiredFocusTargetName
		// is set on the CDO; otherwise reachability is N/A and only the
		// dangling-explicit-target finding stays load-bearing.
		Registry.RegisterAction(
			TEXT("ui"), TEXT("audit_focus_chain"),
			TEXT("Static audit of a WBP's UWidget::Navigation graph. Reports unreachable[], "
				 "dead_ends[], cycles[], dangling_explicit[]. Read-only — does not modify the WBP. "
				 "Reachability BFS runs only when the WBP's CDO exposes DesiredFocusTargetName "
				 "(or InitialFocusTargetName) as an FName UPROPERTY."),
			FMonolithActionHandler::CreateStatic(&HandleAuditFocusChain),
			FParamSchemaBuilder()
				.Required(TEXT("wbp_path"), TEXT("string"), TEXT("Widget Blueprint path (alias: asset_path)"))
				.Build(),
			Cat);
	}
}

#endif // WITH_COMMONUI
