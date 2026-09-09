// SPDX-License-Identifier: MIT
// Automation tests for the `input` namespace: Enhanced Input asset inspection
// (FMonolithGASInputAssetActions) and authoring (FMonolithGASInputAuthoringActions).
//
// Goals:
//   - Both halves of the namespace register, with their bounds published in the schema.
//   - Parameter guards reject out-of-range pages, non-canonical paths and ambiguous selectors.
//   - Reads report page/scan completeness honestly and never dirty a package.
//   - Authored triggers/modifiers are outered into the mapping context's package and are
//     not transient — the two conditions a package save requires to export them.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithInputActionsTestDetail
{
	const TArray<FString> ReadActions = {
		TEXT("list_input_actions"),
		TEXT("get_input_action"),
		TEXT("list_input_mapping_contexts"),
		TEXT("get_input_mapping_context"),
		TEXT("validate_input_mappings")
	};

	const TArray<FString> AuthoringActions = {
		TEXT("add_mapping_modifier"),
		TEXT("add_mapping_trigger"),
		TEXT("remove_mapping_modifier"),
		TEXT("remove_mapping_trigger"),
		TEXT("set_mapping_modifiers"),
		TEXT("set_mapping_triggers"),
		TEXT("set_input_action_modifiers"),
		TEXT("set_input_action_triggers")
	};

	FString MakeObjectPath(const FString& PackagePath)
	{
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	FMonolithActionResult Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("input"), Action, Params);
	}

	int32 GetInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Default = 0)
	{
		double Value = static_cast<double>(Default);
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return static_cast<int32>(Value);
	}

	bool GetBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Default = FString())
	{
		FString Value = Default;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	template <typename TAsset>
	TAsset* CreateAsset(const FString& PackagePath)
	{
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return nullptr;
		}
		TAsset* Asset = NewObject<TAsset>(Package, *FPackageName::GetLongPackageAssetName(PackagePath),
			RF_Public | RF_Standalone | RF_Transactional);
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
		}
		return Asset;
	}

	void CleanupAsset(const FString& PackagePath)
	{
		UObject* Asset = StaticFindObject(UObject::StaticClass(), nullptr, *MakeObjectPath(PackagePath));
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (Asset)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
			{
				Context->UnmapAll();
			}
			FAssetRegistryModule::AssetDeleted(Asset);
			const FName TransientName = MakeUniqueObjectName(GetTransientPackage(), Asset->GetClass(),
				*FString::Printf(TEXT("MONOLITH_INPUT_TEST_%s"), *Asset->GetName()));
			Asset->Rename(*TransientName.ToString(), GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	/** Two input actions and one context that maps both to the same key. */
	struct FScopedInputFixture
	{
		FString Root;
		FString ActionAPath;
		FString ActionBPath;
		FString ContextPath;
		UInputAction* ActionA = nullptr;
		UInputAction* ActionB = nullptr;
		UInputMappingContext* Context = nullptr;

		FScopedInputFixture()
		{
			Root = FString::Printf(TEXT("/Game/Tests/Monolith/InputInspection/%s"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
			ActionAPath = Root + TEXT("/IA_Primary");
			ActionBPath = Root + TEXT("/IA_Secondary");
			ContextPath = Root + TEXT("/IMC_Test");
		}

		~FScopedInputFixture()
		{
			CleanupAsset(ContextPath);
			CleanupAsset(ActionBPath);
			CleanupAsset(ActionAPath);
		}

		bool Create()
		{
			ActionA = CreateAsset<UInputAction>(ActionAPath);
			ActionB = CreateAsset<UInputAction>(ActionBPath);
			Context = CreateAsset<UInputMappingContext>(ContextPath);
			if (!ActionA || !ActionB || !Context)
			{
				return false;
			}

			ActionA->ValueType = EInputActionValueType::Axis2D;
			ActionA->ActionDescription = FText::FromString(TEXT("Primary test action"));
			ActionA->Triggers.Add(NewObject<UInputTriggerPressed>(ActionA));
			ActionA->Modifiers.Add(NewObject<UInputModifierNegate>(ActionA));
			Context->ContextDescription = FText::FromString(TEXT("Enhanced Input fixture"));
			Context->MapKey(ActionA, EKeys::SpaceBar);
			Context->MapKey(ActionB, EKeys::SpaceBar);

			ActionA->GetOutermost()->SetDirtyFlag(false);
			ActionB->GetOutermost()->SetDirtyFlag(false);
			Context->GetOutermost()->SetDirtyFlag(false);
			return true;
		}

		bool PackagesAreClean() const
		{
			return ActionA && ActionB && Context
				&& !ActionA->GetOutermost()->IsDirty()
				&& !ActionB->GetOutermost()->IsDirty()
				&& !Context->GetOutermost()->IsDirty();
		}
	};

	TSharedPtr<FJsonObject> MakeAssetParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration + published bounds
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInputRegistrationTest,
	"Monolith.Input.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInputRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInputActionsTestDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bPassed = true;

	for (const FString& Action : ReadActions)
	{
		bPassed &= TestTrue(*FString::Printf(TEXT("input.%s is registered"), *Action),
			Registry.HasAction(TEXT("input"), Action));
	}
	for (const FString& Action : AuthoringActions)
	{
		bPassed &= TestTrue(*FString::Printf(TEXT("input.%s is registered"), *Action),
			Registry.HasAction(TEXT("input"), Action));
	}
	bPassed &= TestEqual(TEXT("The input namespace owns exactly five read and eight authoring actions"),
		Registry.GetActions(TEXT("input")).Num(), ReadActions.Num() + AuthoringActions.Num());

	// The namespace mixes reads and writes, so the dispatcher must not claim read-only.
	const FMonolithDispatcherAnnotations Annotations = Registry.GetDispatcherAnnotations(TEXT("input"));
	bPassed &= TestFalse(TEXT("Mixed input dispatcher does not claim read-only"), Annotations.bReadOnlyHint);
	bPassed &= TestTrue(TEXT("Input dispatcher declares destructive removals"), Annotations.bDestructiveHint);
	bPassed &= TestFalse(TEXT("Input dispatcher title is set"), Annotations.Title.IsEmpty());

	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("input")))
	{
		if (Info.Action == TEXT("get_input_mapping_context"))
		{
			bPassed &= TestTrue(TEXT("Mapping read schema publishes pagination"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("mapping_offset"))
					&& Info.ParamSchema->HasField(TEXT("mapping_limit")));
		}
		if (Info.Action == TEXT("validate_input_mappings"))
		{
			bPassed &= TestTrue(TEXT("Validation schema publishes scan bounds"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("mapping_scan_limit"))
					&& Info.ParamSchema->HasField(TEXT("limit")));
		}
		if (Info.Action == TEXT("add_mapping_modifier"))
		{
			bPassed &= TestTrue(TEXT("Authoring schema publishes both mapping selectors"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("mapping_index"))
					&& Info.ParamSchema->HasField(TEXT("input_action"))
					&& Info.ParamSchema->HasField(TEXT("key"))
					&& Info.ParamSchema->HasField(TEXT("save")));
		}
	}
	return bPassed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter guards
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInputParamGuardTest,
	"Monolith.Input.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInputParamGuardTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInputActionsTestDetail;

	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		const FMonolithActionResult Result = Execute(TEXT("list_input_actions"), Params);
		bPassed &= TestFalse(TEXT("Zero list limit is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Zero list limit is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 17);
		const FMonolithActionResult Result = Execute(TEXT("get_input_action"), Params);
		bPassed &= TestFalse(TEXT("Non-string asset path is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Non-string asset path is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(TEXT("/Game/Input/IMC_Test.IMC_Other"));
		const FMonolithActionResult Result = Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestFalse(TEXT("Mismatched object leaf is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mismatched object leaf is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(TEXT("/Game/Input/IMC_Test"));
		Params->SetNumberField(TEXT("mapping_limit"), 501);
		const FMonolithActionResult Result = Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestFalse(TEXT("Mapping page over hard cap is rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"), MakeStringArray({ TEXT("/Game/Input/IMC_Test") }));
		Params->SetStringField(TEXT("path"), TEXT("/Game/Input"));
		const FMonolithActionResult Result = Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestFalse(TEXT("Mutually exclusive validation selectors are rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"), TArray<TSharedPtr<FJsonValue>>());
		const FMonolithActionResult Result = Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestFalse(TEXT("Explicit empty context list is rejected"), Result.bSuccess);
	}
	return bPassed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reads: paging, completeness, and the no-mutation contract
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInputReadbackTest,
	"Monolith.Input.ReadbackAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInputReadbackTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInputActionsTestDetail;

	FScopedInputFixture Fixture;
	if (!TestTrue(TEXT("Fixture assets were created"), Fixture.Create()))
	{
		return false;
	}

	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Fixture.Root);
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_details"), true);
		const FMonolithActionResult Result = Execute(TEXT("list_input_actions"), Params);
		bPassed &= TestTrue(TEXT("Input action list succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Input action list total"), GetInt(Result.Result, TEXT("total")), 2);
		bPassed &= TestEqual(TEXT("Input action page count"), GetInt(Result.Result, TEXT("count")), 1);
		bPassed &= TestTrue(TEXT("Input action page reports more"), GetBool(Result.Result, TEXT("has_more")));
	}
	{
		const FMonolithActionResult Result = Execute(TEXT("get_input_action"), MakeAssetParams(Fixture.ActionAPath));
		bPassed &= TestTrue(TEXT("Input action read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Input action value type is preserved"),
			GetString(Result.Result, TEXT("value_type")), FString(TEXT("Axis2D")));
		bPassed &= TestEqual(TEXT("Input action trigger count"), GetInt(Result.Result, TEXT("trigger_count")), 1);
		bPassed &= TestEqual(TEXT("Input action modifier count"), GetInt(Result.Result, TEXT("modifier_count")), 1);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("get_input_mapping_context"), Params);
		bPassed &= TestTrue(TEXT("Mapping context read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mapping context total mappings"), GetInt(Result.Result, TEXT("mapping_count")), 2);
		bPassed &= TestEqual(TEXT("Mapping context returned mappings"), GetInt(Result.Result, TEXT("mappings_returned")), 1);
		bPassed &= TestTrue(TEXT("Mapping context reports truncation"), GetBool(Result.Result, TEXT("mappings_truncated")));
		bPassed &= TestTrue(TEXT("Mapping context reports more"), GetBool(Result.Result, TEXT("has_more_mappings")));
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"), MakeStringArray({ Fixture.ContextPath }));
		Params->SetNumberField(TEXT("mapping_scan_limit"), 100);
		const FMonolithActionResult Result = Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Mapping validation succeeds"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("Mapping validation is complete"), GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestTrue(TEXT("Duplicate-key warning does not invalidate"), GetBool(Result.Result, TEXT("valid")));
		bPassed &= TestEqual(TEXT("Mapping validation error count"), GetInt(Result.Result, TEXT("errors")), 0);
		bPassed &= TestEqual(TEXT("Mapping validation warning count"), GetInt(Result.Result, TEXT("warnings")), 1);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"),
			MakeStringArray({ Fixture.ContextPath, Fixture.Root + TEXT("/ZZ_IMC_Missing") }));
		Params->SetNumberField(TEXT("limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Paginated validation returns structured output"), Result.bSuccess);
		bPassed &= TestTrue(TEXT("Returned validation page is complete"), GetBool(Result.Result, TEXT("page_complete")));
		bPassed &= TestFalse(TEXT("One page does not cover all contexts"),
			GetBool(Result.Result, TEXT("all_contexts_covered"), true));
		bPassed &= TestFalse(TEXT("Partial context coverage cannot claim complete"),
			GetBool(Result.Result, TEXT("complete"), true));
		bPassed &= TestFalse(TEXT("Partial context coverage cannot claim valid"),
			GetBool(Result.Result, TEXT("valid"), true));
		bPassed &= TestTrue(TEXT("Paginated validation reports another page"), GetBool(Result.Result, TEXT("has_more")));
		bPassed &= TestEqual(TEXT("A complete clean page adds no errors"), GetInt(Result.Result, TEXT("errors")), 0);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("context_paths"), MakeStringArray({ Fixture.ContextPath }));
		Params->SetNumberField(TEXT("mapping_scan_limit"), 1);
		const FMonolithActionResult Result = Execute(TEXT("validate_input_mappings"), Params);
		bPassed &= TestTrue(TEXT("Bounded mapping validation returns structured output"), Result.bSuccess);
		bPassed &= TestFalse(TEXT("Bounded mapping validation reports incomplete"),
			GetBool(Result.Result, TEXT("complete"), true));
		bPassed &= TestFalse(TEXT("Incomplete mapping validation cannot claim valid"),
			GetBool(Result.Result, TEXT("valid"), true));
		bPassed &= TestEqual(TEXT("Scan cutoff is an explicit error"), GetInt(Result.Result, TEXT("errors")), 1);
	}

	bPassed &= TestTrue(TEXT("Read-only calls leave fixture packages clean"), Fixture.PackagesAreClean());
	return bPassed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Authoring: outer and flags are what a package save needs
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInputAuthoringTest,
	"Monolith.Input.Authoring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInputAuthoringTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithInputActionsTestDetail;

	FScopedInputFixture Fixture;
	if (!TestTrue(TEXT("Fixture assets were created"), Fixture.Create()))
	{
		return false;
	}

	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
		const FMonolithActionResult Result = Execute(TEXT("add_mapping_modifier"), Params);
		bPassed &= TestTrue(TEXT("Short modifier name resolves and applies"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mapping now holds one modifier"), GetInt(Result.Result, TEXT("modifier_count")), 1);

		const FEnhancedActionKeyMapping& Mapping = Fixture.Context->GetMappings()[0];
		if (TestEqual(TEXT("Modifier landed in the mapping array"), Mapping.Modifiers.Num(), 1)
			&& TestNotNull(TEXT("Modifier is non-null"), Mapping.Modifiers[0].Get()))
		{
			const UInputModifier* Modifier = Mapping.Modifiers[0].Get();
			// The two conditions SavePackage needs to export this subobject rather than
			// write a null: it lives inside the context's package, and it is not transient.
			bPassed &= TestTrue(TEXT("Modifier is outered to the mapping context"),
				Modifier->GetOuter() == Fixture.Context);
			bPassed &= TestTrue(TEXT("Modifier is inside the context package"),
				Modifier->IsInPackage(Fixture.Context->GetOutermost()));
			bPassed &= TestFalse(TEXT("Modifier is not transient"), Modifier->HasAnyFlags(RF_Transient));
			bPassed &= TestTrue(TEXT("Modifier is public"), Modifier->HasAnyFlags(RF_Public));
			bPassed &= TestTrue(TEXT("Modifier is transactional"), Modifier->HasAnyFlags(RF_Transactional));
			bPassed &= TestTrue(TEXT("Modifier is a UInputModifierNegate"),
				Modifier->IsA(UInputModifierNegate::StaticClass()));
		}
		bPassed &= TestTrue(TEXT("Authoring dirties the context package"), Fixture.Context->GetOutermost()->IsDirty());
	}
	{
		// (input_action, key) is ambiguous here: both fixture mappings use SpaceBar,
		// and only IA_Primary + SpaceBar resolves to exactly one mapping.
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetStringField(TEXT("input_action"), Fixture.ActionAPath);
		Params->SetStringField(TEXT("key"), TEXT("SpaceBar"));
		Params->SetStringField(TEXT("trigger_class"), TEXT("Pulse"));
		const FMonolithActionResult Result = Execute(TEXT("add_mapping_trigger"), Params);
		bPassed &= TestTrue(TEXT("Action+key selector resolves a unique mapping"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Trigger was added to mapping 0"), GetInt(Result.Result, TEXT("mapping_index")), 0);
		bPassed &= TestEqual(TEXT("Mapping now holds one trigger"), GetInt(Result.Result, TEXT("trigger_count")), 1);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetStringField(TEXT("input_action"), Fixture.ActionAPath);
		Params->SetStringField(TEXT("key"), TEXT("NotAKeyAtAll"));
		Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
		const FMonolithActionResult Result = Execute(TEXT("add_mapping_modifier"), Params);
		bPassed &= TestFalse(TEXT("Unknown FKey is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Unknown FKey is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetStringField(TEXT("modifier_class"), TEXT("UInputAction"));
		const FMonolithActionResult Result = Execute(TEXT("add_mapping_modifier"), Params);
		bPassed &= TestFalse(TEXT("A non-modifier class is rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bNoSuchProperty"), true);
		Params->SetObjectField(TEXT("properties"), Props);
		const FMonolithActionResult Result = Execute(TEXT("add_mapping_modifier"), Params);
		bPassed &= TestFalse(TEXT("Unknown property name is rejected, not dropped"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mapping modifier count is unchanged after refusal"),
			Fixture.Context->GetMappings()[0].Modifiers.Num(), 1);
	}
	{
		// Replace the whole array, including a property override on the new entry.
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class"), TEXT("InputModifierDeadZone"));
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("LowerThreshold"), 0.25);
		Entry->SetObjectField(TEXT("properties"), Props);

		TArray<TSharedPtr<FJsonValue>> ModifierArray;
		ModifierArray.Add(MakeShared<FJsonValueObject>(Entry));

		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetArrayField(TEXT("modifiers"), ModifierArray);
		const FMonolithActionResult Result = Execute(TEXT("set_mapping_modifiers"), Params);
		bPassed &= TestTrue(TEXT("set_mapping_modifiers replaces the array"), Result.bSuccess);

		const FEnhancedActionKeyMapping& Mapping = Fixture.Context->GetMappings()[0];
		if (TestEqual(TEXT("Only the replacement modifier remains"), Mapping.Modifiers.Num(), 1))
		{
			const UInputModifierDeadZone* DeadZone = Cast<UInputModifierDeadZone>(Mapping.Modifiers[0].Get());
			if (TestNotNull(TEXT("Replacement modifier is a dead zone"), DeadZone))
			{
				bPassed &= TestEqual(TEXT("Property override was applied"), DeadZone->LowerThreshold, 0.25f);
				bPassed &= TestTrue(TEXT("Replacement is outered to the mapping context"),
					DeadZone->GetOuter() == Fixture.Context);
			}
		}
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
		const FMonolithActionResult Result = Execute(TEXT("remove_mapping_modifier"), Params);
		bPassed &= TestFalse(TEXT("Removing an absent class reports failure"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetNumberField(TEXT("index"), 0);
		Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
		const FMonolithActionResult Result = Execute(TEXT("remove_mapping_modifier"), Params);
		bPassed &= TestFalse(TEXT("index and modifier_class together are rejected"), Result.bSuccess);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ContextPath);
		Params->SetNumberField(TEXT("mapping_index"), 0);
		Params->SetNumberField(TEXT("index"), 0);
		const FMonolithActionResult Result = Execute(TEXT("remove_mapping_modifier"), Params);
		bPassed &= TestTrue(TEXT("Removal by index succeeds"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mapping modifier array is now empty"),
			Fixture.Context->GetMappings()[0].Modifiers.Num(), 0);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeAssetParams(Fixture.ActionBPath);
		Params->SetArrayField(TEXT("triggers"), MakeStringArray({ TEXT("Pressed"), TEXT("InputTriggerHold") }));
		const FMonolithActionResult Result = Execute(TEXT("set_input_action_triggers"), Params);
		bPassed &= TestTrue(TEXT("set_input_action_triggers succeeds"), Result.bSuccess);
		if (TestEqual(TEXT("Input action holds both triggers"), Fixture.ActionB->Triggers.Num(), 2)
			&& TestNotNull(TEXT("First trigger is non-null"), Fixture.ActionB->Triggers[0].Get()))
		{
			bPassed &= TestTrue(TEXT("Trigger is outered to the input action"),
				Fixture.ActionB->Triggers[0]->GetOuter() == Fixture.ActionB);
			bPassed &= TestFalse(TEXT("Trigger is not transient"),
				Fixture.ActionB->Triggers[0]->HasAnyFlags(RF_Transient));
		}
	}
	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
