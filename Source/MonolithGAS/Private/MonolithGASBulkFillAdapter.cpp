// SPDX-License-Identifier: MIT
// MonolithGASBulkFillAdapter — Phase 2 adapter for the FMonolithBulkFillRegistry.
// Routes "gas" target_namespace traffic from the central bulk_fill.apply /
// describe.schema dispatchers to:
//
//   * a FAttributeMetaData DataTable row writer (the 20-attr × 10-level = 200-cell
//     grind reduced to one call — design B.10 / plan §Phase 2),
//   * an FGameplayAttribute resolver that pre-validates every column-name in the
//     tree against the supplied `attribute_set` BEFORE commit (rename hazard —
//     design "Cross-Cutting Engine Quirks" row),
//   * a describe tree that inlines the modifier-magnitude tagged-union shape
//     (ScalableFloat / AttributeBased / SetByCaller / CustomCalculation per
//     design Cross-Cutting Engine Quirks row).
//
// H5 stub-adapter invariant: Register() runs unconditionally from StartupModule.
// The adapter BODY switches on WITH_GBA — the dev build wires the real handlers,
// the release/no-GBA build wires a stub that returns a clean error so the
// `monolith_discover("gas")` action surface stays identical across build flavours.
//
// NOTE: AttributeInit DataTable writes do NOT need the
// MonolithBlueprintEditCradle (transient-subobject reparent + FOverridableManager
// inner-property override). DataTables don't carry instanced-subobject UPROPERTY
// containers the way Blueprint CDOs do — Phase 1's cradle was Blueprint-specific.
// A plain FScopedTransaction + Modify() + AddRow + MarkPackageDirty is the
// reflection-correct sequence here.

#include "MonolithGASBulkFillAdapter.h"
#include "MonolithGASInternal.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "Engine/DataTable.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#if WITH_GBA
// AttributeSet.h is provided by the always-on GameplayAbilities module (NOT GBA),
// but FAttributeMetaData and FGameplayAttribute live in that header. The WITH_GBA
// gate here is the H5 invariant — the body itself is exercised whenever GAS is
// linked, which is the default. The WITH_GBA=0 stub branch is the
// release-build-without-GAS exit door.
#include "AttributeSet.h"
#include "GameplayEffect.h"
#endif

namespace MonolithGASBulkFillInternal
{
	// Build a single-error FDryRunReport for use when target resolution / validation
	// fails. Keeps the dispatcher's return shape uniform whether the walker ran or not.
	// Mirrors MonolithBlueprintBulkFillAdapter.cpp::MakeResolveFailureReport.
	static FDryRunReport MakeResolveFailureReport(const FString& Reason)
	{
		FDryRunReport Report;
		FBulkFillFieldWrite Write;
		Write.Path = TEXT("(adapter)");
		Write.bOk = false;
		Write.Reason = Reason;
		Report.FieldWrites.Add(Write);
		Report.Errors = 1;
		Report.bWouldApply = false;
		return Report;
	}

#if WITH_GBA
	// Determine if an asset path looks like a Blueprint path (starts with /) vs a
	// C++ class name. Mirrors the helper at
	// MonolithGASAttributeActions.cpp:217-220 (private to that TU).
	static bool LooksLikeBlueprintPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/"));
	}

	// Find a C++ UAttributeSet subclass by name. Mirrors the helper at
	// MonolithGASAttributeActions.cpp:455-478 (private to that TU). Accepts both
	// "ULeviathanVitalsSet" and "LeviathanVitalsSet" forms.
	static UClass* FindAttributeSetClass(const FString& ClassName)
	{
		FString SearchName = ClassName;
		if (!SearchName.StartsWith(TEXT("U")))
		{
			SearchName = TEXT("U") + SearchName;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(UAttributeSet::StaticClass()))
			{
				const FString Name = Class->GetName();
				if (Name == SearchName || (TEXT("U") + Name) == SearchName || Name == ClassName)
				{
					return Class;
				}
			}
		}
		return nullptr;
	}

	// Resolve the `attribute_set` tree param into a UClass*. Accepts either:
	//   - C++ class name ("ULeviathanVitalsSet" / "LeviathanVitalsSet"), OR
	//   - Blueprint asset path ("/Game/.../BP_VitalsSet").
	// Returns nullptr if neither path resolves. Caller emits a (adapter)-level error.
	static UClass* ResolveAttributeSetClass(const FString& AttrSet)
	{
		if (AttrSet.IsEmpty()) return nullptr;
		if (LooksLikeBlueprintPath(AttrSet))
		{
			UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AttrSet);
			if (UBlueprint* BP = Cast<UBlueprint>(Asset))
			{
				if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UAttributeSet::StaticClass()))
				{
					return BP->GeneratedClass;
				}
			}
			return nullptr;
		}
		return FindAttributeSetClass(AttrSet);
	}

	// Resolve an attribute column-name (e.g. "MaxHealth") to its FProperty on the
	// AttributeSet class. The rename-hazard surface from design "Cross-Cutting Engine
	// Quirks" — if the AttributeSet renamed `Health → HP`, the tree's "Health" key
	// returns nullptr here and we emit a SilentDrops entry rather than silently
	// writing a dead row.
	static FProperty* ResolveAttributeOnSet(UClass* SetClass, const FString& AttrName)
	{
		if (!SetClass) return nullptr;
		// Exact match first, then case-insensitive — mirrors the
		// FindPropertyForwarding pattern at MonolithReflectionWalker.h:65.
		if (FProperty* P = SetClass->FindPropertyByName(FName(*AttrName))) return P;
		for (TFieldIterator<FProperty> It(SetClass); It; ++It)
		{
			if (It->GetName().Equals(AttrName, ESearchCase::IgnoreCase)) return *It;
		}
		return nullptr;
	}

	// Locate FAttributeMetaData. Returns nullptr if the GameplayAbilities module
	// isn't loaded (defensive — WITH_GBA gate above already implies it is, but
	// keep this null-safe).
	static UScriptStruct* GetAttributeMetaDataStruct()
	{
		return FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayAbilities.AttributeMetaData"));
	}

	// Write a single FAttributeMetaData row from a JSON value (number or object).
	//
	// Number-form: { "Player.1": { "MaxHealth": 100.0 } } — sets BaseValue only,
	//              MinValue/MaxValue default-constructed (matches existing
	//              create_attribute_init_datatable behaviour at
	//              MonolithGASAttributeActions.cpp:2270-2298).
	//
	// Object-form: { "Player.1": { "MaxHealth": { "base": 100.0, "min": 0.0, "max": 9999.0 } } }
	//              — sets BaseValue/MinValue/MaxValue from named keys.
	//
	// Emits a FBulkFillFieldWrite into OutReport per cell. Honors Spec.bDryRun
	// (skip AddRow) and Spec.bStrict (mark Report.bWouldApply=false on any error).
	static void WriteAttributeCell(
		UDataTable* DataTable,
		UScriptStruct* RowStruct,
		UClass* SetClass,
		const FString& RowName,
		const FString& AttrName,
		const TSharedPtr<FJsonValue>& CellVal,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport)
	{
		FBulkFillFieldWrite Write;
		Write.Path = FString::Printf(TEXT("rows[%s].%s"), *RowName, *AttrName);
		Write.bOk = false;

		// Rename-hazard pre-check — must succeed BEFORE we touch the row data.
		FProperty* AttrProp = ResolveAttributeOnSet(SetClass, AttrName);
		if (!AttrProp)
		{
			Write.Reason = FString::Printf(
				TEXT("attribute '%s' not found on set %s — possible rename hazard, ")
				TEXT("FGameplayAttribute would resolve to nullptr at runtime"),
				*AttrName, *SetClass->GetName());
			OutReport.FieldWrites.Add(Write);
			OutReport.SilentDrops.Add(Write); // surface as a rename-hazard warning
			OutReport.Errors++;
			return;
		}

		// Decode the JSON cell — number or object form.
		float BaseValue = 0.f, MinValue = 0.f, MaxValue = 0.f;
		bool bHaveMin = false, bHaveMax = false;
		double NumVal = 0.0;
		if (CellVal->TryGetNumber(NumVal))
		{
			BaseValue = static_cast<float>(NumVal);
		}
		else
		{
			const TSharedPtr<FJsonObject>* CellObj = nullptr;
			if (!CellVal->TryGetObject(CellObj) || !CellObj || !CellObj->IsValid())
			{
				Write.Reason = TEXT("cell value must be a number or {base, min, max} object");
				OutReport.FieldWrites.Add(Write);
				OutReport.Errors++;
				return;
			}
			(*CellObj)->TryGetNumberField(TEXT("base"), NumVal);  BaseValue = static_cast<float>(NumVal); NumVal = 0.0;
			bHaveMin = (*CellObj)->TryGetNumberField(TEXT("min"), NumVal); MinValue = static_cast<float>(NumVal); NumVal = 0.0;
			bHaveMax = (*CellObj)->TryGetNumberField(TEXT("max"), NumVal); MaxValue = static_cast<float>(NumVal);
		}

		Write.ProposedValue = FString::Printf(TEXT("base=%f min=%f max=%f"), BaseValue, MinValue, MaxValue);

		// Dry-run: record the would-write and stop.
		if (Spec.bDryRun)
		{
			Write.bOk = true;
			OutReport.FieldWrites.Add(Write);
			return;
		}

		// Commit: allocate + initialise + write the row, then AddRow with overwrite-aware
		// semantics. Mirrors MonolithGASAttributeActions.cpp:2270-2298.
		uint8* RowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
		RowStruct->InitializeStruct(RowData);

		// Row-name format here is `[Group].[AttributeSet].[Attribute]` per
		// FAttributeSetInitterDiscreteLevels semantics (AttributeSet.h:303-318).
		// The tree gives the row prefix (e.g. "Player.1"); we append `.<AttrSet>.<Attr>`.
		const FString FullRowName = FString::Printf(TEXT("%s.%s.%s"),
			*RowName, *SetClass->GetName(), *AttrName);

		if (FProperty* BaseProp = RowStruct->FindPropertyByName(TEXT("BaseValue")))
		{
			BaseProp->CopyCompleteValue(BaseProp->ContainerPtrToValuePtr<void>(RowData), &BaseValue);
		}
		if (bHaveMin)
		{
			if (FProperty* MinProp = RowStruct->FindPropertyByName(TEXT("MinValue")))
			{
				MinProp->CopyCompleteValue(MinProp->ContainerPtrToValuePtr<void>(RowData), &MinValue);
			}
		}
		if (bHaveMax)
		{
			if (FProperty* MaxProp = RowStruct->FindPropertyByName(TEXT("MaxValue")))
			{
				MaxProp->CopyCompleteValue(MaxProp->ContainerPtrToValuePtr<void>(RowData), &MaxValue);
			}
		}

		// Overwrite-aware: AddRow internally handles "already present" by replacing.
		DataTable->AddRow(FName(*FullRowName), RowData, RowStruct);

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);

		Write.bOk = true;
		OutReport.FieldWrites.Add(Write);
		if (!OutReport.WouldModify.Contains(FullRowName))
		{
			OutReport.WouldModify.Add(FullRowName);
		}
	}

	// Build the FSchemaDescriptor subtree for a modifier-magnitude tagged union
	// (ScalableFloat / AttributeBased / SetByCaller / CustomCalculation). Surfaced
	// from the gas describe adapter per design Cross-Cutting Engine Quirks row.
	static FSchemaDescriptor BuildModifierMagnitudeDescriptor()
	{
		FSchemaDescriptor Desc;
		Desc.FieldPath = TEXT("modifier_magnitude");
		Desc.TypeName = TEXT("TaggedUnion<EGameplayEffectMagnitudeCalculation>");
		Desc.ImportTextForm = TEXT("(MagnitudeCalculationType=ScalableFloat,ScalableFloatMagnitude=(Value=10.0))");
		Desc.EnumValues = {
			TEXT("ScalableFloat"),
			TEXT("AttributeBased"),
			TEXT("SetByCaller"),
			TEXT("CustomCalculationClass")
		};

		FSchemaDescriptor ScalableFloat;
		ScalableFloat.FieldPath = TEXT("ScalableFloatMagnitude");
		ScalableFloat.TypeName = TEXT("FScalableFloat");
		ScalableFloat.ConditionalOn = TEXT("MagnitudeCalculationType=ScalableFloat");
		ScalableFloat.ImportTextForm = TEXT("(Value=10.0,Curve=(CurveTable=None,RowName=None))");
		Desc.Children.Add(ScalableFloat);

		FSchemaDescriptor AttributeBased;
		AttributeBased.FieldPath = TEXT("AttributeBasedMagnitude");
		AttributeBased.TypeName = TEXT("FAttributeBasedFloat");
		AttributeBased.ConditionalOn = TEXT("MagnitudeCalculationType=AttributeBased");
		AttributeBased.ImportTextForm = TEXT("(BackingAttribute=(Attribute=None,AttributeSource=Source),Coefficient=(Value=1.0))");
		Desc.Children.Add(AttributeBased);

		FSchemaDescriptor SetByCaller;
		SetByCaller.FieldPath = TEXT("SetByCallerMagnitude");
		SetByCaller.TypeName = TEXT("FSetByCallerFloat");
		SetByCaller.ConditionalOn = TEXT("MagnitudeCalculationType=SetByCaller");
		SetByCaller.ImportTextForm = TEXT("(DataTag=(TagName=\"Data.Damage\"))");
		Desc.Children.Add(SetByCaller);

		FSchemaDescriptor CustomCalc;
		CustomCalc.FieldPath = TEXT("CustomMagnitude");
		CustomCalc.TypeName = TEXT("FCustomCalculationBasedFloat");
		CustomCalc.ConditionalOn = TEXT("MagnitudeCalculationType=CustomCalculationClass");
		CustomCalc.ImportTextForm = TEXT("(CalculationClassMagnitude=None,Coefficient=(Value=1.0))");
		Desc.Children.Add(CustomCalc);

		return Desc;
	}

	// Build the FSchemaDescriptor for an FAttributeMetaData row (the AttributeInit
	// DT row struct). Self-contained — does not require a target asset.
	static FSchemaDescriptor BuildAttributeMetaDataDescriptor()
	{
		FSchemaDescriptor Desc;
		Desc.FieldPath = TEXT("FAttributeMetaData");
		Desc.TypeName = TEXT("USTRUCT");
		Desc.ImportTextForm = TEXT("(BaseValue=100.0,MinValue=0.0,MaxValue=9999.0,bCanStack=false)");

		auto AddChild = [&](const TCHAR* Name, const TCHAR* Type, const TCHAR* Sample)
		{
			FSchemaDescriptor Child;
			Child.FieldPath = Name;
			Child.TypeName = Type;
			Child.ImportTextForm = Sample;
			Desc.Children.Add(Child);
		};
		AddChild(TEXT("BaseValue"),            TEXT("float"),   TEXT("100.0"));
		AddChild(TEXT("MinValue"),             TEXT("float"),   TEXT("0.0"));
		AddChild(TEXT("MaxValue"),             TEXT("float"),   TEXT("9999.0"));
		AddChild(TEXT("DerivedAttributeInfo"), TEXT("FString"), TEXT("\"\""));
		AddChild(TEXT("bCanStack"),            TEXT("bool"),    TEXT("false"));
		return Desc;
	}
#endif // WITH_GBA
}

// ---------------------------------------------------------------------------
// FBulkFillAdapter — invoked from bulk_fill.apply for target_namespace="gas"
// ---------------------------------------------------------------------------
FDryRunReport FMonolithGASBulkFillAdapter::GasBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithGASBulkFillInternal;

#if WITH_GBA
	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("gas adapter: spec.tree is null"));
	}

	// --- fill_kind dispatcher. v1 supports AttributeInitDataTable only.
	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		FillKind = TEXT("AttributeInitDataTable");
	}
	if (FillKind != TEXT("AttributeInitDataTable"))
	{
		return MakeResolveFailureReport(FString::Printf(
			TEXT("gas adapter: unsupported fill_kind '%s' (v1 supports AttributeInitDataTable only)"),
			*FillKind));
	}

	// --- Resolve the DataTable target.
	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
	UDataTable* DataTable = Cast<UDataTable>(Asset);
	if (!DataTable)
	{
		return MakeResolveFailureReport(FString::Printf(
			TEXT("gas adapter: asset is not a UDataTable: %s"), *Spec.TargetAsset));
	}

	UScriptStruct* MetaStruct = GetAttributeMetaDataStruct();
	if (!MetaStruct)
	{
		return MakeResolveFailureReport(TEXT("gas adapter: FAttributeMetaData struct not found — GameplayAbilities module missing?"));
	}
	if (DataTable->RowStruct != MetaStruct)
	{
		return MakeResolveFailureReport(FString::Printf(
			TEXT("gas adapter: DataTable RowStruct is %s, expected FAttributeMetaData"),
			DataTable->RowStruct ? *DataTable->RowStruct->GetName() : TEXT("(null)")));
	}

	// --- Resolve `attribute_set` (rename-hazard scope).
	FString AttrSetParam;
	Spec.Tree->TryGetStringField(TEXT("attribute_set"), AttrSetParam);
	UClass* SetClass = ResolveAttributeSetClass(AttrSetParam);
	if (!SetClass)
	{
		return MakeResolveFailureReport(FString::Printf(
			TEXT("gas adapter: attribute_set '%s' did not resolve to a UAttributeSet subclass"),
			*AttrSetParam));
	}

	// --- Extract rows tree.
	const TSharedPtr<FJsonObject>* RowsObjPtr = nullptr;
	if (!Spec.Tree->TryGetObjectField(TEXT("rows"), RowsObjPtr) || !RowsObjPtr || !RowsObjPtr->IsValid())
	{
		return MakeResolveFailureReport(TEXT("gas adapter: spec.tree.rows missing or not an object"));
	}
	const TSharedPtr<FJsonObject>& RowsObj = *RowsObjPtr;

	FDryRunReport Report;

	// --- Dry-run path: walk without mutation. We still validate every attribute
	// resolves against `attribute_set` so the rename hazard surfaces in the report.
	if (Spec.bDryRun)
	{
		for (const auto& RowKV : RowsObj->Values)
		{
			const FString& RowName = RowKV.Key;
			const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
			if (!RowKV.Value->TryGetObject(RowObjPtr) || !RowObjPtr || !RowObjPtr->IsValid())
			{
				FBulkFillFieldWrite W;
				W.Path = FString::Printf(TEXT("rows[%s]"), *RowName);
				W.Reason = TEXT("row value must be an object of {AttrName: value}");
				W.bOk = false;
				Report.FieldWrites.Add(W);
				Report.Errors++;
				continue;
			}
			for (const auto& CellKV : (*RowObjPtr)->Values)
			{
				WriteAttributeCell(DataTable, MetaStruct, SetClass, RowName, CellKV.Key, CellKV.Value, Spec, Report);
			}
		}
		Report.bWouldApply = false;
		return Report;
	}

	// --- Commit path: single transaction wrapping the whole 200-cell write.
	DataTable->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithGASBulkFillAdapter", "GasBulkFill",
		"Monolith GAS Bulk Fill"));
	DataTable->Modify();

	for (const auto& RowKV : RowsObj->Values)
	{
		const FString& RowName = RowKV.Key;
		const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
		if (!RowKV.Value->TryGetObject(RowObjPtr) || !RowObjPtr || !RowObjPtr->IsValid())
		{
			FBulkFillFieldWrite W;
			W.Path = FString::Printf(TEXT("rows[%s]"), *RowName);
			W.Reason = TEXT("row value must be an object of {AttrName: value}");
			W.bOk = false;
			Report.FieldWrites.Add(W);
			Report.Errors++;
			continue;
		}
		for (const auto& CellKV : (*RowObjPtr)->Values)
		{
			WriteAttributeCell(DataTable, MetaStruct, SetClass, RowName, CellKV.Key, CellKV.Value, Spec, Report);
		}
	}

	// Strict-mode contract: any error rejects the whole batch and rolls back.
	if (Spec.bStrict && Report.Errors > 0)
	{
		Transaction.Cancel();
		Report.bWouldApply = false;
		return Report;
	}

	DataTable->MarkPackageDirty();
	Report.bWouldApply = true;
	return Report;

#else // WITH_GBA
	// H5 stub-adapter branch — keeps `monolith_discover("gas")` action surface
	// identical across dev + release builds, returns a clean error rather than
	// "unknown namespace" so callers can detect availability programmatically.
	return MakeResolveFailureReport(TEXT(
		"gas adapter: GAS optional dep not available (WITH_GBA=0). "
		"Build with the BlueprintAttributes / GameplayAbilities plugin to enable AttributeInit DT bulk_fill."));
#endif
}

// ---------------------------------------------------------------------------
// FDescribeAdapter — invoked from describe.schema for target_namespace="gas"
// ---------------------------------------------------------------------------
FSchemaDescriptor FMonolithGASBulkFillAdapter::GasDescribe(const FString& TargetAsset)
{
	using namespace MonolithGASBulkFillInternal;

#if WITH_GBA
	// Asset-aware describe: if TargetAsset names a DataTable with FAttributeMetaData
	// row struct, return the row-struct schema; otherwise emit the modifier-magnitude
	// tagged-union descriptor (the GE describe surface). Empty TargetAsset → describe
	// both shapes as a top-level union so callers can pick.

	if (!TargetAsset.IsEmpty())
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(TargetAsset);
		if (UDataTable* DT = Cast<UDataTable>(Asset))
		{
			UScriptStruct* MetaStruct = GetAttributeMetaDataStruct();
			if (DT->RowStruct == MetaStruct)
			{
				FSchemaDescriptor Out = BuildAttributeMetaDataDescriptor();
				Out.FieldPath = TargetAsset;
				return Out;
			}
		}
		// Treat any other asset path as a GE-shaped describe (modifier-magnitude).
		// A future asset-class router can branch here; for v1 we collapse to the
		// tagged-union surface which is the documented gas describe target in
		// plan §Phase 2 Step 2.
		FSchemaDescriptor GEDesc;
		GEDesc.FieldPath = TargetAsset;
		GEDesc.TypeName = TEXT("UGameplayEffect");
		GEDesc.ImportTextForm = TEXT("(see Modifiers array)");
		GEDesc.Children.Add(BuildModifierMagnitudeDescriptor());
		return GEDesc;
	}

	// Empty TargetAsset → return a top-level descriptor with both shapes as
	// children. Callers introspecting "what can gas bulk_fill?" see the union.
	FSchemaDescriptor Root;
	Root.FieldPath = TEXT("gas");
	Root.TypeName = TEXT("Namespace");
	Root.ImportTextForm = TEXT("(see children for fill_kind shapes)");
	Root.Children.Add(BuildAttributeMetaDataDescriptor());
	Root.Children.Add(BuildModifierMagnitudeDescriptor());
	return Root;

#else // WITH_GBA
	// H5 stub: emit an error-shaped descriptor so callers see availability.
	FSchemaDescriptor Empty;
	Empty.FieldPath = TEXT("(adapter)");
	Empty.TypeName = TEXT("error");
	Empty.ImportTextForm = TEXT(
		"gas describe: GAS optional dep not available (WITH_GBA=0). "
		"Build with the BlueprintAttributes / GameplayAbilities plugin to enable.");
	return Empty;
#endif
}

// ---------------------------------------------------------------------------
// Registration entry-points (called from FMonolithGASModule::StartupModule
// / ShutdownModule).
//
// **H5 invariant**: Register() runs unconditionally — the body switches on
// WITH_GBA, not the registration. This guarantees `monolith_discover("gas")`
// returns the same row in dev and release builds.
// ---------------------------------------------------------------------------
void FMonolithGASBulkFillAdapter::Register()
{
	// FBulkFillAdapter / FDescribeAdapter are TFunction<>; pass function pointers
	// directly. CreateStatic is delegate-class API and would not compile here
	// (verified during Phase 1 against the plan-snippet shorthand).
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("gas"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithGASBulkFillAdapter::GasBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithGASBulkFillAdapter::GasDescribe));
}

void FMonolithGASBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("gas"));
}
