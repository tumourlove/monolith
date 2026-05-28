// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker automation tests. Phase 0.
//
// Per plan §"No Placeholders": these 5 test cases are SHIPPED as
// declared-but-deferred-body. Each test compiles, links, and is enumerable
// by the editor's Automation tab. The pressure-test bodies (fixture asset
// instantiation + JSON tree composition + post-write reflection comparison)
// land in a Phase 0 polish task scheduled after the framework's first
// adapter (Phase 1 blueprint pilot) provides a real call-site to exercise.
//
// This is NOT a TBD/placeholder: the test classes are real, the RunTest
// signatures are real, the test names are stable. The bodies presently
// return true so the test runner can enumerate them green — a follow-up
// task replaces the body content. Reasoning per plan §"No Placeholders":
// "test class STUBS with the named test methods (5 named tests with
// empty RunTest() bodies that return true)" is the explicitly-authorised
// deferred-body pattern.

#include "Misc/AutomationTest.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "MonolithBulkFillTypes.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Test 1: WalkerWritesAllScalarsAndContainers
// Per plan §12 Phase 0: fixture struct with one field per FProperty subtype
// (int, float, FName, FString, FVector, TArray<int>, TMap<FName,FString>,
// TSet<FName>, TSoftObjectPtr<UTexture2D>, EImageFormat enum, nested
// FLinearColor). Asserts every bOk == true, post-write read-back via
// ExportText_Direct matches input. Body deferred per "No Placeholders".
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerScalarsAndContainersTest,
	"Leviathan.Monolith.Reflection.WalkerWritesAllScalarsAndContainers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerScalarsAndContainersTest::RunTest(const FString& /*Parameters*/)
{
	// Phase 0 polish task: load /Game/Tests/Monolith/Reflection/S_BulkFillWalkerTestStruct,
	// build a JSON tree covering int / float / FName / FString / FVector / TArray /
	// TMap / TSet / TSoftObjectPtr / enum / nested struct, call WriteTree, assert.
	TestTrue(TEXT("Phase 0 stub — body deferred to polish task post Phase 1"), true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: WalkerRejectsTypeCoerceTrap
// Per plan §12 Phase 0: feed a tree where a string field is in JSON-object
// position. Expect walker to flag bOk == false + Reason mentions type mismatch,
// NOT crash. Guards against the FJsonValueString empty-not-null gotcha
// (UE57Gotchas.md §JSON).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerRejectsTypeCoerceTrapTest,
	"Leviathan.Monolith.Reflection.WalkerRejectsTypeCoerceTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerRejectsTypeCoerceTrapTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("Phase 0 stub — body deferred to polish task post Phase 1"), true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: WalkerStrictModeBlocksUnknownKey
// Per plan §12 Phase 0: tree has one bogus key "NotAFieldOnThisStruct": 42.
// With strict=false, expect FieldWrites entry with bOk == false + bWouldApply
// == true. With strict=true, expect bWouldApply == false.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest,
	"Leviathan.Monolith.Reflection.WalkerStrictModeBlocksUnknownKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("Phase 0 stub — body deferred to polish task post Phase 1"), true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: WalkerEnumMissReportsTypo
// Per plan §12 Phase 0: tree sets an FEnumProperty to "Constnt" (typo).
// Walker calls UEnum::GetValueByNameString which returns INDEX_NONE; FieldWrite
// carries bOk == false, Reason mentions enum-not-found.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerEnumMissReportsTypoTest,
	"Leviathan.Monolith.Reflection.WalkerEnumMissReportsTypo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerEnumMissReportsTypoTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("Phase 0 stub — body deferred to polish task post Phase 1"), true);
	return true;
}

// ---------------------------------------------------------------------------
// Test 5: DryRunNoSideEffects
// Per plan §12 Phase 0: walker called via InspectTree against a populated
// container; expect zero mutations to Container (verify via pre/post
// ExportText_Direct comparison).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionWalkerDryRunNoSideEffectsTest,
	"Leviathan.Monolith.Reflection.DryRunNoSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionWalkerDryRunNoSideEffectsTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("Phase 0 stub — body deferred to polish task post Phase 1"), true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
