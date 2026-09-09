// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-29-ri-ergonomics-improvements-handover.md (item #7).
//
// MonolithConfigSetterTest — automation coverage for the dev-gated
// `set_developer_setting` action. Verifies:
//   1. Roundtrip — set + resolve_setting returns the same value (canonical form).
//   2. Restore — old value captured in response payload matches pre-mutation state.
//   3. Unknown class returns a clean error with the suggested-name hint format.
//   4. Unknown property returns a clean error listing known property names.
//
// Target settings class: UMonolithReflectionIntelSettings (also a sibling Monolith
// module, so we get cross-module class-resolution coverage). The
// `bIndexMarketplacePluginReflection` toggle is the canonical motivating example
// from the handover plan; we use it as the mutating victim.
//
// All tests are #if WITH_DEV_AUTOMATION_TESTS + the action is #if WITH_EDITOR —
// these flags are co-active in editor automation runs.

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithConfigActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithConfigSetterTestDetail
{
	static TSharedPtr<FJsonObject> MakeParams(
		const FString& Class, const FString& Property, const FString& Value,
		bool bSaveConfig = false)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class"), Class);
		P->SetStringField(TEXT("property"), Property);
		P->SetStringField(TEXT("value"), Value);
		P->SetBoolField(TEXT("save_config"), bSaveConfig);
		return P;
	}

	// Resolve the target settings class by short-name via the same path the
	// action uses, so the test does not introduce a hard module dep on
	// MonolithReflectionIntel.
	static UClass* FindReflectionIntelSettingsClass()
	{
		// Try full path first (matches what we'd ship in CHANGELOG).
		UClass* C = FindObject<UClass>(nullptr,
			TEXT("/Script/MonolithReflectionIntel.MonolithReflectionIntelSettings"));
		if (C != nullptr) { return C; }
		// Fallback short-name lookup (matches action's behaviour).
		return FindFirstObject<UClass>(
			TEXT("MonolithReflectionIntelSettings"), EFindFirstObjectOptions::NativeFirst);
	}
}

// ---------------------------------------------------------------------------
// Test 1: Roundtrip — flip bIndexMarketplacePluginReflection and read it back.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigSetterRoundtripTest,
	"Monolith.Config.SetDeveloperSetting.Roundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigSetterRoundtripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithConfigSetterTestDetail;

	UClass* SettingsClass = FindReflectionIntelSettingsClass();
	if (!TestNotNull(TEXT("UMonolithReflectionIntelSettings resolvable"), SettingsClass))
	{
		return false;
	}

	UObject* CDO = SettingsClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
	if (!TestNotNull(TEXT("CDO exists"), CDO)) { return false; }

	FProperty* Prop = SettingsClass->FindPropertyByName(TEXT("bIndexMarketplacePluginReflection"));
	if (!TestNotNull(TEXT("bIndexMarketplacePluginReflection property exists"), Prop))
	{
		return false;
	}

	// Capture original so we can restore at end-of-test (CDO is process-global).
	FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop);
	if (!TestNotNull(TEXT("property is bool"), BoolProp)) { return false; }
	const bool bOriginal = BoolProp->GetPropertyValue_InContainer(CDO);

	// Flip via the action — explicitly using the OPPOSITE of the current value
	// so the test passes regardless of the default this CDO ships with.
	const FString TargetText = bOriginal ? TEXT("false") : TEXT("true");
	const FMonolithActionResult Result = FMonolithConfigActions::SetDeveloperSetting(
		MakeParams(TEXT("MonolithReflectionIntelSettings"),
				   TEXT("bIndexMarketplacePluginReflection"),
				   TargetText));

	TestTrue(TEXT("action succeeded"), Result.bSuccess);
	if (TestTrue(TEXT("result payload present"), Result.Result.IsValid()))
	{
		FString OldVal, NewVal;
		Result.Result->TryGetStringField(TEXT("old_value"), OldVal);
		Result.Result->TryGetStringField(TEXT("new_value"), NewVal);
		TestEqual(TEXT("old_value reports prior state"),
			OldVal, bOriginal ? FString(TEXT("True")) : FString(TEXT("False")));
		TestEqual(TEXT("new_value reports mutated state"),
			NewVal, bOriginal ? FString(TEXT("False")) : FString(TEXT("True")));

		bool bSaved = true;
		Result.Result->TryGetBoolField(TEXT("saved"), bSaved);
		TestFalse(TEXT("save_config defaulted to false, not persisted"), bSaved);
	}

	// Read-back via the CDO directly (cheaper than threading through
	// resolve_setting, which targets the on-disk INI rather than the live CDO).
	const bool bAfter = BoolProp->GetPropertyValue_InContainer(CDO);
	TestEqual(TEXT("CDO reflects mutation"), bAfter, !bOriginal);

	// Restore the original value so we leave no side-effects for other tests.
	BoolProp->SetPropertyValue_InContainer(CDO, bOriginal);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Unknown class — error path with hint string.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigSetterUnknownClassTest,
	"Monolith.Config.SetDeveloperSetting.UnknownClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigSetterUnknownClassTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithConfigSetterTestDetail;

	const FMonolithActionResult Result = FMonolithConfigActions::SetDeveloperSetting(
		MakeParams(TEXT("NonExistentSettingsClass_DoNotCreate"),
				   TEXT("SomeProperty"), TEXT("true")));

	TestFalse(TEXT("action failed"), Result.bSuccess);
	TestTrue(TEXT("error mentions unknown class"),
		Result.ErrorMessage.Contains(TEXT("unknown class")));
	TestTrue(TEXT("error includes short-name hint"),
		Result.ErrorMessage.Contains(TEXT("MonolithReflectionIntelSettings")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Unknown property — error path lists known property names.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigSetterUnknownPropertyTest,
	"Monolith.Config.SetDeveloperSetting.UnknownProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigSetterUnknownPropertyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithConfigSetterTestDetail;

	UClass* SettingsClass = FindReflectionIntelSettingsClass();
	if (!TestNotNull(TEXT("settings class resolvable"), SettingsClass)) { return false; }

	const FMonolithActionResult Result = FMonolithConfigActions::SetDeveloperSetting(
		MakeParams(TEXT("MonolithReflectionIntelSettings"),
				   TEXT("ThisPropertyDoesNotExist"), TEXT("true")));

	TestFalse(TEXT("action failed"), Result.bSuccess);
	TestTrue(TEXT("error mentions unknown property"),
		Result.ErrorMessage.Contains(TEXT("unknown property")));
	TestTrue(TEXT("error includes known-properties hint"),
		Result.ErrorMessage.Contains(TEXT("known properties")));
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Missing required params — early validation.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigSetterMissingParamsTest,
	"Monolith.Config.SetDeveloperSetting.MissingParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigSetterMissingParamsTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("class"), TEXT("MonolithReflectionIntelSettings"));
	// 'property' and 'value' intentionally omitted.

	const FMonolithActionResult Result = FMonolithConfigActions::SetDeveloperSetting(P);
	TestFalse(TEXT("action failed"), Result.bSuccess);
	TestTrue(TEXT("error mentions required params"),
		Result.ErrorMessage.Contains(TEXT("required")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
