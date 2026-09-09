// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithStructFieldTests.cpp
//
// Tests for the struct-field editing actions (get / add / remove / rename /
// set_type on an existing UUserDefinedStruct).
//
// The fixture is a UUserDefinedStruct created into an in-memory package under
// /Game/Tests/Monolith/ and never saved. That is enough for the handlers to
// resolve it: FMonolithAssetUtils::LoadAssetByPath's tier 3 is
// FindPackage + FindObject, which finds a freshly-created unsaved asset. Every
// writer call passes skip_save so nothing reaches disk, and the fixture is
// trashed at the end of each test (same pattern as MonolithPinTypeGrammarTest).
//
// WHAT THESE LOCK:
//   1. The ENGINE behaviours the handlers are built on, each of which was read
//      out of StructureEditorUtils.cpp while writing them and would break a
//      handler silently if it changed:
//        - RemoveVariable refuses to empty a struct (bAllowToMakeEmpty is
//          hardcoded false) and returns false for BOTH that and "not found",
//          which is why remove_struct_field pre-checks the count.
//        - RenameVariable preserves VarGuid -- the reason a rename does not
//          disconnect existing Break/Make pins.
//        - AddVariable appends, so "the new field is the last description".
//        - MoveVariable(PositionBelow) reorders, which `after` depends on.
//        - ChangeVariableType returns FALSE for an unchanged type, which is
//          indistinguishable from a refusal unless the no-op is answered first.
//   2. The get -> add round-trip: every type get_struct_fields reports parses
//      back through add_struct_field to the same reported type. This is the
//      property that makes the read action useful as an input to the writers,
//      and it is what catches a container (array/set/map) losing a half.
//   3. Action-level behaviour and validation.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithBlueprintStructActions.h"
#include "MonolithPinTypeGrammar.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/Guid.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace MonolithStructFieldTests
{
	/** A struct in an in-memory package -- resolvable by path, never saved. */
	struct FFixture
	{
		UPackage* Package = nullptr;
		UUserDefinedStruct* Struct = nullptr;
		FString Path;

		bool IsValid() const { return Package && Struct; }

		void Destroy()
		{
			if (Struct)
			{
				Struct->ClearFlags(RF_Standalone | RF_Public);
				Struct->RemoveFromRoot();
				Struct->MarkAsGarbage();
				Struct = nullptr;
			}
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->ClearFlags(RF_Standalone);
				Package->RemoveFromRoot();
				Package->MarkAsGarbage();
				Package = nullptr;
			}
		}
	};

	static FFixture MakeFixture(const TCHAR* Tag)
	{
		FFixture Out;
		// GUID-suffixed: CreatePackage returns an EXISTING in-memory package for a
		// repeated path, so a fixed name would hand the second run the first run's
		// half-torn-down struct.
		const FString Name = FString::Printf(TEXT("S_MonolithFieldFixture_%s_%s"),
			Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Out.Path = FString::Printf(TEXT("/Game/Tests/Monolith/structfields/%s"), *Name);

		Out.Package = CreatePackage(*Out.Path);
		if (!Out.Package)
		{
			return Out;
		}
		Out.Struct = FStructureEditorUtils::CreateUserDefinedStruct(
			Out.Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
		return Out;
	}

	static TSharedPtr<FJsonObject> Params(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), AssetPath);
		// Never touch disk from a test.
		P->SetBoolField(TEXT("skip_save"), true);
		return P;
	}

	/** The display name of the single member CreateUserDefinedStruct seeds. */
	static FString SeededFieldName(UUserDefinedStruct* Struct)
	{
		const TArray<FStructVariableDescription>* Desc = FStructureEditorUtils::GetVarDescPtr(Struct);
		return (Desc && Desc->Num() > 0) ? (*Desc)[0].FriendlyName : FString();
	}

	/** Collect name -> type from a get_struct_fields payload, in declaration order. */
	static bool ReadFields(const FMonolithActionResult& Result,
		TArray<TPair<FString, FString>>& OutFields)
	{
		OutFields.Reset();
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Result.Result->TryGetArrayField(TEXT("fields"), Arr) || !Arr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V.IsValid() && V->TryGetObject(Obj) && Obj && (*Obj).IsValid())
			{
				OutFields.Emplace((*Obj)->GetStringField(TEXT("name")), (*Obj)->GetStringField(TEXT("type")));
			}
		}
		return true;
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
	using namespace MonolithStructFieldTests;

	FFixture Fx = MakeFixture(TEXT("Contract"));
	if (!TestTrue(TEXT("fixture struct is created"), Fx.IsValid()))
	{
		Fx.Destroy();
		return false;
	}
	UUserDefinedStruct* Struct = Fx.Struct;

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
	const FEdGraphPinType StringType = MonolithPinTypeGrammar::ParsePinTypeFromString(TEXT("string"));
	TestTrue(TEXT("AddVariable succeeds"), FStructureEditorUtils::AddVariable(Struct, StringType));
	TestEqual(TEXT("AddVariable appended"),
		FStructureEditorUtils::GetVarDesc(Struct).Num(), 2);
	const FGuid AddedGuid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
	TestNotEqual(TEXT("the appended field is a distinct member"), AddedGuid, OnlyGuid);

	// ChangeVariableType reports FALSE when the requested type is the one the
	// field already has. set_struct_field_type answers that no-op itself, because
	// at the call site it is indistinguishable from a refusal.
	TestFalse(TEXT("ChangeVariableType returns false for an unchanged type"),
		FStructureEditorUtils::ChangeVariableType(Struct, AddedGuid, StringType));

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
	TestTrue(TEXT("AddVariable (second)"), FStructureEditorUtils::AddVariable(
		Struct, MonolithPinTypeGrammar::ParsePinTypeFromString(TEXT("int"))));
	TestTrue(TEXT("AddVariable (third)"), FStructureEditorUtils::AddVariable(
		Struct, MonolithPinTypeGrammar::ParsePinTypeFromString(TEXT("bool"))));
	if (TestEqual(TEXT("three fields before the move"),
			FStructureEditorUtils::GetVarDesc(Struct).Num(), 3))
	{
		const FGuid LastGuid  = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
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

	Fx.Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Test 2: get_struct_fields output feeds straight back into add_struct_field.
//
// The read action is only useful as an input to the writers if the type grammar
// survives the trip. Containers are where it breaks: PinTypeToString alone
// reports array:int as plain `int`, and "map:" plus a key type alone parses back
// as "a map with no value type".
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStructFieldRoundTripTest,
	"Monolith.StructFields.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStructFieldRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithStructFieldTests;

	FFixture Source = MakeFixture(TEXT("RoundTripSrc"));
	FFixture Dest   = MakeFixture(TEXT("RoundTripDst"));
	if (!TestTrue(TEXT("both fixtures are created"), Source.IsValid() && Dest.IsValid()))
	{
		Source.Destroy();
		Dest.Destroy();
		return false;
	}

	const TArray<FString> Types = {
		TEXT("bool"), TEXT("int"), TEXT("int64"), TEXT("float"), TEXT("double"),
		TEXT("string"), TEXT("name"), TEXT("text"), TEXT("byte"),
		TEXT("struct:Vector"), TEXT("object:StaticMesh"),
		TEXT("array:int"), TEXT("set:name"), TEXT("map:string:int"),
	};

	int32 Index = 0;
	for (const FString& TypeStr : Types)
	{
		TSharedPtr<FJsonObject> P = Params(Source.Path);
		P->SetStringField(TEXT("name"), FString::Printf(TEXT("Field%d"), Index++));
		P->SetStringField(TEXT("type"), TypeStr);
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		TestTrue(FString::Printf(TEXT("add_struct_field accepts %s (%s)"), *TypeStr, *R.ErrorMessage),
			R.bSuccess);
	}

	TArray<TPair<FString, FString>> SourceFields;
	TestTrue(TEXT("get_struct_fields reads the source struct"),
		ReadFields(FMonolithBlueprintStructActions::HandleGetStructFields(Params(Source.Path)), SourceFields));

	// Feed every reported type back in. A type that does not round-trip fails
	// here rather than silently producing a differently-typed field.
	for (const TPair<FString, FString>& Field : SourceFields)
	{
		TSharedPtr<FJsonObject> P = Params(Dest.Path);
		P->SetStringField(TEXT("name"), Field.Key);
		P->SetStringField(TEXT("type"), Field.Value);
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		TestTrue(FString::Printf(TEXT("the reported type '%s' is accepted back (%s)"),
			*Field.Value, *R.ErrorMessage), R.bSuccess);
	}

	TArray<TPair<FString, FString>> DestFields;
	TestTrue(TEXT("get_struct_fields reads the destination struct"),
		ReadFields(FMonolithBlueprintStructActions::HandleGetStructFields(Params(Dest.Path)), DestFields));

	// Both structs carry their own seeded member first, so compare by name.
	for (const TPair<FString, FString>& Field : SourceFields)
	{
		const TPair<FString, FString>* Match = DestFields.FindByPredicate(
			[&Field](const TPair<FString, FString>& D) { return D.Key == Field.Key; });
		if (TestNotNull(*FString::Printf(TEXT("%s exists on the round-tripped struct"), *Field.Key), Match))
		{
			TestEqual(FString::Printf(TEXT("%s keeps its reported type"), *Field.Key),
				Match->Value, Field.Value);
		}
	}

	Source.Destroy();
	Dest.Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Test 3: action-level behaviour on a live struct.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithStructFieldEditingTest,
	"Monolith.StructFields.Editing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithStructFieldEditingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithStructFieldTests;

	FFixture Fx = MakeFixture(TEXT("Editing"));
	if (!TestTrue(TEXT("fixture struct is created"), Fx.IsValid()))
	{
		Fx.Destroy();
		return false;
	}

	// --- add -----------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Health"));
		P->SetStringField(TEXT("type"), TEXT("float"));
		P->SetStringField(TEXT("default_value"), TEXT("100.0"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		if (TestTrue(FString::Printf(TEXT("add Health (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestEqual(TEXT("the struct now has two fields"),
				(int32)R.Result->GetNumberField(TEXT("field_count")), 2);
			TestTrue(TEXT("the response reports the recompile"), R.Result->GetBoolField(TEXT("recompiled")));
			TestFalse(TEXT("skip_save means nothing was written"), R.Result->GetBoolField(TEXT("saved")));
		}
	}

	// A duplicate is refused by name, and the refusal points at the two actions
	// that CAN change an existing field.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Health"));
		P->SetStringField(TEXT("type"), TEXT("int"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		TestFalse(TEXT("a duplicate field name is refused"), R.bSuccess);
		TestTrue(TEXT("the refusal names the alternatives"),
			R.ErrorMessage.Contains(TEXT("rename_struct_field")));
	}

	// A bogus type token is a caller error, not a silently-created bool field.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Bogus"));
		P->SetStringField(TEXT("type"), TEXT("struct:NoSuchStructAnywhere"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		TestFalse(TEXT("an unresolvable type token is refused"), R.bSuccess);
		TestTrue(TEXT("the error quotes the offending token"),
			R.ErrorMessage.Contains(TEXT("NoSuchStructAnywhere")));

		TArray<TPair<FString, FString>> Fields;
		ReadFields(FMonolithBlueprintStructActions::HandleGetStructFields(Params(Fx.Path)), Fields);
		TestFalse(TEXT("the refused field was not created"),
			Fields.ContainsByPredicate([](const TPair<FString, FString>& F) { return F.Key == TEXT("Bogus"); }));
	}

	// --- after ---------------------------------------------------------------
	// Anchored on the LAST field, so AddVariable's append already satisfies the
	// request and MoveVariable has nothing to do. MoveVariable returns false in
	// exactly that case (ComputeIndicesForMove bails on InitialIndex == NewIndex),
	// so a handler that trusted its return value would report a correctly-placed
	// field as misplaced.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Armour"));
		P->SetStringField(TEXT("type"), TEXT("int"));
		P->SetStringField(TEXT("after"), TEXT("Health"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		if (TestTrue(FString::Printf(TEXT("add Armour after Health (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestTrue(TEXT("an already-correct placement is reported as placed"),
				R.Result->GetBoolField(TEXT("positioned_after")));

			TArray<TPair<FString, FString>> Fields;
			ReadFields(FMonolithBlueprintStructActions::HandleGetStructFields(Params(Fx.Path)), Fields);
			int32 HealthIdx = INDEX_NONE, ArmourIdx = INDEX_NONE;
			for (int32 i = 0; i < Fields.Num(); ++i)
			{
				if (Fields[i].Key == TEXT("Health")) { HealthIdx = i; }
				if (Fields[i].Key == TEXT("Armour")) { ArmourIdx = i; }
			}
			TestTrue(TEXT("Armour sits directly after Health"),
				HealthIdx != INDEX_NONE && ArmourIdx == HealthIdx + 1);
		}
	}

	// Anchored on the FIRST field, which needs a genuine reorder.
	{
		const FString Seeded = SeededFieldName(Fx.Struct);
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Shield"));
		P->SetStringField(TEXT("type"), TEXT("int"));
		P->SetStringField(TEXT("after"), Seeded);
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		if (TestTrue(FString::Printf(TEXT("add Shield after the first field (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestTrue(TEXT("the reorder is reported as placed"),
				R.Result->GetBoolField(TEXT("positioned_after")));

			TArray<TPair<FString, FString>> Fields;
			ReadFields(FMonolithBlueprintStructActions::HandleGetStructFields(Params(Fx.Path)), Fields);
			if (TestTrue(TEXT("the struct has four fields"), Fields.Num() == 4))
			{
				TestEqual(TEXT("Shield was moved into second place"), Fields[1].Key, FString(TEXT("Shield")));
			}
		}

		// Put the struct back to [seeded, Health, Armour] for the steps below.
		TSharedPtr<FJsonObject> Rm = Params(Fx.Path);
		Rm->SetStringField(TEXT("name"), TEXT("Shield"));
		TestTrue(TEXT("remove Shield"),
			FMonolithBlueprintStructActions::HandleRemoveStructField(Rm).bSuccess);
	}

	// An `after` that names nothing lists what does exist.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Stamina"));
		P->SetStringField(TEXT("type"), TEXT("float"));
		P->SetStringField(TEXT("after"), TEXT("NoSuchField"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleAddStructField(P);
		TestFalse(TEXT("an unknown anchor is refused"), R.bSuccess);
		TestTrue(TEXT("the refusal lists the fields that do exist"),
			R.ErrorMessage.Contains(TEXT("Health")));
	}

	// --- rename --------------------------------------------------------------
	{
		FString GuidBefore;
		{
			const FMonolithActionResult Get =
				FMonolithBlueprintStructActions::HandleGetStructFields(Params(Fx.Path));
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Get.bSuccess && Get.Result->TryGetArrayField(TEXT("fields"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					const TSharedPtr<FJsonObject>* Obj = nullptr;
					if (V->TryGetObject(Obj) && (*Obj)->GetStringField(TEXT("name")) == TEXT("Health"))
					{
						GuidBefore = (*Obj)->GetStringField(TEXT("guid"));
					}
				}
			}
		}

		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Health"));
		P->SetStringField(TEXT("new_name"), TEXT("HitPoints"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRenameStructField(P);
		if (TestTrue(FString::Printf(TEXT("rename Health -> HitPoints (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestTrue(TEXT("the rename reports a change"), R.Result->GetBoolField(TEXT("changed")));
			TestEqual(TEXT("the previous name is reported"),
				R.Result->GetStringField(TEXT("previous_name")), FString(TEXT("Health")));
			// The GUID surviving is why existing Break/Make pins keep their links.
			TestEqual(TEXT("the GUID survives the rename"),
				R.Result->GetStringField(TEXT("guid")), GuidBefore);
		}
	}

	// Renaming onto an existing name is refused rather than producing two fields
	// a caller cannot tell apart.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("HitPoints"));
		P->SetStringField(TEXT("new_name"), TEXT("Armour"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRenameStructField(P);
		TestFalse(TEXT("renaming onto an existing name is refused"), R.bSuccess);
	}

	// Renaming a field to the name it already has is a reported no-op, not the
	// engine's bare false.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("HitPoints"));
		P->SetStringField(TEXT("new_name"), TEXT("HitPoints"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRenameStructField(P);
		if (TestTrue(TEXT("a same-name rename succeeds"), R.bSuccess))
		{
			TestFalse(TEXT("and reports that nothing changed"), R.Result->GetBoolField(TEXT("changed")));
		}
	}

	// --- set type ------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("HitPoints"));
		P->SetStringField(TEXT("type"), TEXT("int"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleSetStructFieldType(P);
		if (TestTrue(FString::Printf(TEXT("retype HitPoints to int (%s)"), *R.ErrorMessage), R.bSuccess))
		{
			TestTrue(TEXT("the retype reports a change"), R.Result->GetBoolField(TEXT("changed")));
			TestEqual(TEXT("the previous type is reported"),
				R.Result->GetStringField(TEXT("previous_type")), FString(TEXT("float")));
			// ChangeVariableType assigns DefaultValue = FString() -- the 100.0 set at
			// creation is gone, not converted, and the response has to say so.
			TestTrue(TEXT("the discarded default value is reported"),
				R.Result->GetBoolField(TEXT("default_value_cleared")));
		}
	}

	// Retyping to the type the field already has is a reported no-op. Without
	// this branch the engine's false would surface as a spurious refusal.
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("HitPoints"));
		P->SetStringField(TEXT("type"), TEXT("int"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleSetStructFieldType(P);
		if (TestTrue(TEXT("a same-type retype succeeds"), R.bSuccess))
		{
			TestFalse(TEXT("and reports that nothing changed"), R.Result->GetBoolField(TEXT("changed")));
		}
	}

	// --- remove --------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("NotAField"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRemoveStructField(P);
		TestFalse(TEXT("removing a field that does not exist fails"), R.bSuccess);
		TestTrue(TEXT("the failure lists the fields that do exist"),
			R.ErrorMessage.Contains(TEXT("HitPoints")));
	}
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("Armour"));
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRemoveStructField(P);
		TestTrue(FString::Printf(TEXT("remove Armour (%s)"), *R.ErrorMessage), R.bSuccess);
	}

	// Strip down to one field, then confirm the last one cannot go -- and that
	// the caller is told WHICH failure this is, since the engine returns the same
	// false for "no such field".
	{
		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), TEXT("HitPoints"));
		TestTrue(TEXT("remove HitPoints"),
			FMonolithBlueprintStructActions::HandleRemoveStructField(P).bSuccess);
	}
	{
		const FString LastField = SeededFieldName(Fx.Struct);
		TestFalse(TEXT("a seeded field name was found"), LastField.IsEmpty());

		TSharedPtr<FJsonObject> P = Params(Fx.Path);
		P->SetStringField(TEXT("name"), LastField);
		const FMonolithActionResult R = FMonolithBlueprintStructActions::HandleRemoveStructField(P);
		TestFalse(TEXT("removing the last remaining field is refused"), R.bSuccess);
		TestTrue(TEXT("the refusal explains that a struct cannot be empty"),
			R.ErrorMessage.Contains(TEXT("cannot be left empty")));
	}

	Fx.Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// Test 4: parameter validation (returns before any asset is touched).
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
		TestFalse(FString::Printf(TEXT("%s explains itself"), What), Result.ErrorMessage.IsEmpty());
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

	// The writers need their own parameters too, and say which one is missing.
	{
		using namespace MonolithStructFieldTests;
		FFixture Fx = MakeFixture(TEXT("Validation"));
		if (Fx.IsValid())
		{
			ExpectFailure(TEXT("add_struct_field without name/type"),
				FMonolithBlueprintStructActions::HandleAddStructField(Params(Fx.Path)));
			ExpectFailure(TEXT("remove_struct_field without name"),
				FMonolithBlueprintStructActions::HandleRemoveStructField(Params(Fx.Path)));
			ExpectFailure(TEXT("rename_struct_field without new_name"),
				FMonolithBlueprintStructActions::HandleRenameStructField(Params(Fx.Path)));
			ExpectFailure(TEXT("set_struct_field_type without type"),
				FMonolithBlueprintStructActions::HandleSetStructFieldType(Params(Fx.Path)));
		}
		Fx.Destroy();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
