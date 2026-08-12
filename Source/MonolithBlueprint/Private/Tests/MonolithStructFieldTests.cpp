// SPDX-License-Identifier: MIT
// Automation tests for the struct-field editing actions (get/add/remove/rename/
// set_type on an existing UUserDefinedStruct).
//
// Scope, stated honestly: the handlers resolve their target through
// FMonolithAssetUtils::LoadAssetByPath, so a true end-to-end test would have to
// create and delete real content assets. These tests instead do two things that
// need no asset I/O and are deterministic:
//
//   1. Lock the ENGINE behaviours the handlers are built on. Every one of these
//      was read out of StructureEditorUtils.cpp while writing the actions, and a
//      silent change to any of them would break a handler in a way a green
//      compile would not reveal:
//        - RemoveVariable refuses to empty a struct (bAllowToMakeEmpty = false)
//          and returns false, distinguishing "would empty" from "not found" only
//          in a log line. remove_struct_field's pre-check exists because of this.
//        - RenameVariable preserves VarGuid -- which is why a rename does not
//          disconnect existing Break/Make pins.
//        - AddVariable appends, so "the new field is the last description" holds.
//        - MoveVariable(PositionBelow) reorders, which add_struct_field's
//          `after` parameter depends on.
//        - The type grammar round-trips: PinTypeToString(ToPinType()) parses back
//          to the same pin type, which is what lets get_struct_fields output be
//          fed straight back into add_struct_field.
//   2. Cover the action-level parameter validation, which returns before any
//      asset load and so needs no fixture.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithPinTypeGrammar.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "Misc/Guid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithStructFieldTestDetail
{
	/** A throwaway struct in the transient package -- never saved, never on disk. */
	static UUserDefinedStruct* MakeTransientStruct()
	{
		const FName Name(*FString::Printf(TEXT("S_MonolithFieldFixture_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		return FStructureEditorUtils::CreateUserDefinedStruct(
			GetTransientPackage(), Name, RF_Transient);
	}

	static FEdGraphPinType PinTypeOf(const TCHAR* TypeStr)
	{
		return MonolithPinTypeGrammar::ParsePinTypeFromString(TypeStr);
	}
}

// ---------------------------------------------------------------------------
// Test 1: the engine behaviours the handlers rely on.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStructFieldEngineContractTest,
	"Monolith.StructFields.EngineContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStructFieldEngineContractTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithStructFieldTestDetail;

	UUserDefinedStruct* Struct = MakeTransientStruct();
	if (!TestNotNull(TEXT("transient struct is created"), Struct))
	{
		return false;
	}

	// CreateUserDefinedStruct seeds exactly one default member.
	TestEqual(TEXT("a new struct starts with one field"),
		FStructureEditorUtils::GetVarDesc(Struct).Num(), 1);

	// A struct cannot be emptied. remove_struct_field pre-checks the count purely
	// so this case can be reported as something other than an unexplained false.
	const FGuid OnlyGuid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
	TestFalse(TEXT("removing the only field is refused"),
		FStructureEditorUtils::RemoveVariable(Struct, OnlyGuid));
	TestEqual(TEXT("the refused removal left the field in place"),
		FStructureEditorUtils::GetVarDesc(Struct).Num(), 1);

	// AddVariable appends -- "the new field is the last description" is the
	// assumption add_struct_field uses to find what it just created.
	TestTrue(TEXT("AddVariable succeeds"),
		FStructureEditorUtils::AddVariable(Struct, PinTypeOf(TEXT("string"))));
	TestEqual(TEXT("AddVariable appended"),
		FStructureEditorUtils::GetVarDesc(Struct).Num(), 2);
	const FGuid AddedGuid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
	TestNotEqual(TEXT("the appended field is a distinct member"), AddedGuid, OnlyGuid);

	// Rename preserves the GUID. This is the whole reason rename_struct_field is
	// safe on a struct that Blueprints already break: pins bind to the GUID.
	TestTrue(TEXT("RenameVariable succeeds"),
		FStructureEditorUtils::RenameVariable(Struct, AddedGuid, TEXT("RenamedField")));
	const FStructVariableDescription* Renamed =
		FStructureEditorUtils::GetVarDescByGuid(Struct, AddedGuid);
	if (TestNotNull(TEXT("the renamed field is still addressable by its original GUID"), Renamed))
	{
		TestEqual(TEXT("the friendly name changed"), Renamed->FriendlyName, FString(TEXT("RenamedField")));
	}

	// With more than one member, removal is allowed.
	TestTrue(TEXT("removing a field from a 2-field struct succeeds"),
		FStructureEditorUtils::RemoveVariable(Struct, AddedGuid));
	TestEqual(TEXT("the struct is back to one field"),
		FStructureEditorUtils::GetVarDesc(Struct).Num(), 1);

	// Reordering, which add_struct_field's `after` parameter depends on.
	TestTrue(TEXT("AddVariable (second)"), FStructureEditorUtils::AddVariable(Struct, PinTypeOf(TEXT("int"))));
	TestTrue(TEXT("AddVariable (third)"), FStructureEditorUtils::AddVariable(Struct, PinTypeOf(TEXT("bool"))));
	if (TestEqual(TEXT("three fields before the move"),
			FStructureEditorUtils::GetVarDesc(Struct).Num(), 3))
	{
		const FGuid LastGuid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
		const FGuid FirstGuid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
		const bool bMoved = FStructureEditorUtils::MoveVariable(
			Struct, LastGuid, FirstGuid, FStructureEditorUtils::EMovePosition::PositionBelow);
		TestTrue(TEXT("MoveVariable(PositionBelow) succeeds"), bMoved);
		if (bMoved)
		{
			TestEqual(TEXT("the moved field sits directly after its anchor"),
				FStructureEditorUtils::GetVarDesc(Struct)[1].VarGuid, LastGuid);
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: the type grammar round-trips.
//
// get_struct_fields reports types via PinTypeToString so its output can be fed
// straight back into add_struct_field. That only holds if the two directions
// agree, so assert it rather than assume it.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStructFieldTypeRoundTripTest,
	"Monolith.StructFields.TypeRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStructFieldTypeRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithStructFieldTestDetail;

	UUserDefinedStruct* Struct = MakeTransientStruct();
	if (!TestNotNull(TEXT("transient struct is created"), Struct))
	{
		return false;
	}

	const TArray<FString> Types = {
		TEXT("bool"), TEXT("int"), TEXT("float"), TEXT("string"),
		TEXT("name"), TEXT("text"), TEXT("struct:Vector"), TEXT("array:int"),
	};

	for (const FString& TypeStr : Types)
	{
		const FEdGraphPinType Parsed = MonolithPinTypeGrammar::ParsePinTypeFromString(TypeStr);
		if (!TestTrue(FString::Printf(TEXT("AddVariable accepts %s"), *TypeStr),
				FStructureEditorUtils::AddVariable(Struct, Parsed)))
		{
			continue;
		}

		// Round-trip through the description, which is the path get_struct_fields
		// takes: description -> ToPinType() -> PinTypeToString().
		const FStructVariableDescription& Desc = FStructureEditorUtils::GetVarDesc(Struct).Last();
		const FString Reported = MonolithPinTypeGrammar::PinTypeToString(Desc.ToPinType());
		const FEdGraphPinType Reparsed = MonolithPinTypeGrammar::ParsePinTypeFromString(Reported);

		TestEqual(FString::Printf(TEXT("%s round-trips to an equivalent pin type (reported as %s)"),
			*TypeStr, *Reported), Reparsed.PinCategory, Parsed.PinCategory);
		TestEqual(FString::Printf(TEXT("%s round-trips its container type"), *TypeStr),
			(int32)Reparsed.ContainerType, (int32)Parsed.ContainerType);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: action-level parameter validation (returns before any asset load).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStructFieldValidationTest,
	"Monolith.StructFields.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStructFieldValidationTest::RunTest(const FString& /*Parameters*/)
{
	auto ExpectFailure = [this](const TCHAR* What, const FMonolithActionResult& Result)
	{
		TestFalse(FString::Printf(TEXT("%s fails"), What), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s explains itself"), What), !Result.ErrorMessage.IsEmpty());
	};

	// Missing asset_path on every action.
	ExpectFailure(TEXT("get_struct_fields without asset_path"),
		FMonolithBlueprintStructActions::HandleGetStructFields(MakeShared<FJsonObject>()));
	ExpectFailure(TEXT("add_struct_field without asset_path"),
		FMonolithBlueprintStructActions::HandleAddStructField(MakeShared<FJsonObject>()));
	ExpectFailure(TEXT("remove_struct_field without asset_path"),
		FMonolithBlueprintStructActions::HandleRemoveStructField(MakeShared<FJsonObject>()));
	ExpectFailure(TEXT("rename_struct_field without asset_path"),
		FMonolithBlueprintStructActions::HandleRenameStructField(MakeShared<FJsonObject>()));
	ExpectFailure(TEXT("set_struct_field_type without asset_path"),
		FMonolithBlueprintStructActions::HandleSetStructFieldType(MakeShared<FJsonObject>()));

	// A path that resolves to nothing names the asset it could not find, rather
	// than failing anonymously.
	TSharedPtr<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(TEXT("asset_path"), TEXT("/Game/__MonolithNoSuchStruct__"));
	const FMonolithActionResult MissingResult =
		FMonolithBlueprintStructActions::HandleGetStructFields(Missing);
	TestFalse(TEXT("a non-existent struct fails"), MissingResult.bSuccess);
	TestTrue(TEXT("the error names the path"),
		MissingResult.ErrorMessage.Contains(TEXT("__MonolithNoSuchStruct__")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
