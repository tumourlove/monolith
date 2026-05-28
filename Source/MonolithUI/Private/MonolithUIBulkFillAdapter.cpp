// SPDX-License-Identifier: MIT
// MonolithUIBulkFillAdapter — Phase 4 adapter for the FMonolithBulkFillRegistry.
// Routes "ui" target_namespace traffic from the central bulk_fill.apply /
// describe.schema dispatchers to:
//
//   * an FCommonInputActionDataBase DataTable row writer that commits the 40-row
//     KBM/Gamepad-XBox/Gamepad-PS5 binding tree in ONE FScopedTransaction
//     (closes the 2026-04-25 parallel-burst editor-crash mode — design B.11),
//   * a generic UDataTable row writer for vanilla FTableRowBase row-structs
//     (mesh/audio/animation rows-DT pattern shared across Phase 5 namespaces),
//   * a UWidget-property writer for UUserWidget descendants resolved by
//     widget_name inside a WBP's WidgetTree,
//   * a SLOT-PROPERTY describe scoped to the resolved widget's PARENT panel
//     class (UCanvasPanelSlot vs UVerticalBoxSlot vs UOverlaySlot vs
//     UHorizontalBoxSlot vs UUniformGridSlot vs UStackBoxSlot) — slot props are
//     per-parent in UE 5.7, NOT per-widget; this is the worst UI ergonomic pain.
//
// H5 stub-adapter invariant: Register() runs unconditionally from
// FMonolithUIModule::StartupModule. The adapter BODY splits inside the function:
// vanilla-UMG paths (DT row writes against arbitrary FTableRowBase, slot-prop
// describe, widget-property describe) are NOT gated; CommonUI-specific paths
// (FCommonInputActionDataBase rows, activatable stacks, common buttons,
// CommonInput key bindings) are `#if WITH_COMMONUI` gated with a clean stub
// error in the `#else` branch.
//
// SINGLE-TRANSACTION INVARIANT (2026-04-25 parallel-burst crash mitigation):
// ALL row-batch writes (input-action DT + vanilla DT) go through ONE
// FScopedTransaction + ONE Modify() + N AddRow + ONE MarkPackageDirty. A
// process-wide FScopeLock against `GUIBulkFillCriticalSection` serialises
// concurrent UI-row-write attempts so parallel JSON-RPC bursts can no longer
// race against the editor's DataTable mutation cradle.

#include "MonolithUIBulkFillAdapter.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "MonolithAssetUtils.h"
#include "MonolithUICommon.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/OverlaySlot.h"
#include "Components/Overlay.h"
#include "Components/UniformGridSlot.h"
#include "Components/UniformGridPanel.h"
#include "Components/StackBoxSlot.h"
#include "Components/StackBox.h"
#include "WidgetBlueprint.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"

#if WITH_COMMONUI
// CommonUITypes carries `struct FCommonInputActionDataBase : public FTableRowBase`
// at CommonUI/Public/CommonUITypes.h:100 (verified offline source_query, 2026-05-11).
// The struct is the canonical row type for CommonUI's UDataTable<input-action> bindings.
#include "CommonUITypes.h"
#endif

#define LOCTEXT_NAMESPACE "MonolithUIBulkFillAdapter"

namespace
{
	/**
	 * Process-wide critical section serialising UI bulk_fill calls.
	 * Mitigates the 2026-04-25 parallel-burst editor crash: prior implementation
	 * opened N separate FScopedTransactions inside a parallel JSON-RPC burst,
	 * which raced against the DataTable mutation cradle and crashed the editor
	 * on row 40. The single-transaction invariant + this lock cooperate to
	 * guarantee atomicity AND serialisation of UI row-batch writes.
	 *
	 * Granularity: process-wide (NOT per-asset). Concurrent bulk_fill against
	 * different DTs are still serialised — but the crash signature was a row
	 * COUNT issue, not a unique-asset issue. Per-asset locking can be a v1.1
	 * refinement once the crash mode is closed by the single-transaction fix.
	 */
	static FCriticalSection GUIBulkFillCriticalSection;
}

namespace MonolithUIBulkFillInternal
{
	// Build a single-error FDryRunReport for use when target resolution / validation
	// fails. Mirrors MonolithGASBulkFillAdapter.cpp::MakeResolveFailureReport
	// (the shared per-namespace adapter precedent).
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

	// ─────────────────────────────────────────────────────────────────────────
	// Asset resolvers
	// ─────────────────────────────────────────────────────────────────────────

	// Resolve `TargetAsset` to a UDataTable*. Returns nullptr + populated OutError
	// on miss. Caller decides whether the row-struct must be FCommonInputActionDataBase
	// (CommonUI path) or merely FTableRowBase-derived (vanilla path).
	static UDataTable* ResolveDataTable(const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		UDataTable* DT = Cast<UDataTable>(Asset);
		if (!DT)
		{
			OutError = FString::Printf(
				TEXT("asset at '%s' is not a UDataTable (got %s)"),
				*AssetPath,
				Asset ? *Asset->GetClass()->GetName() : TEXT("nullptr"));
		}
		return DT;
	}

	// Resolve a WBP path + widget_name to a UWidget* inside the WBP's WidgetTree.
	// Returns nullptr + populated OutError on miss. The widget lives on the
	// UBaseWidgetBlueprint::WidgetTree (verified
	// Engine\Source\Editor\UnrealEd\Private\BaseWidgetBlueprint.cpp:11) which is
	// `WITH_EDITORONLY_DATA` — we are editor-tooling, so this is the right hook.
	static UWidget* ResolveWidgetInWBP(
		const FString& WBPPath,
		const FString& WidgetName,
		FString& OutError)
	{
		FMonolithActionResult LoadErr;
		UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(WBPPath, LoadErr);
		if (!WBP)
		{
			OutError = LoadErr.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("could not load UWidgetBlueprint at '%s'"), *WBPPath)
				: LoadErr.ErrorMessage;
			return nullptr;
		}
		if (!WBP->WidgetTree)
		{
			OutError = FString::Printf(TEXT("WBP '%s' has no WidgetTree"), *WBPPath);
			return nullptr;
		}
		UWidget* Found = WBP->WidgetTree->FindWidget(FName(*WidgetName));
		if (!Found)
		{
			OutError = FString::Printf(
				TEXT("widget '%s' not found inside WBP '%s'"),
				*WidgetName, *WBPPath);
		}
		return Found;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Per-row writer: walks the JSON row tree against the row-struct's FProperty
	// hierarchy via ImportText_Direct (matches the Phase 0 reflection walker
	// shape used by Phase 1/2/3). Used by both InputActionDataTable and
	// vanilla DataTableRows paths — the row-struct identity is the only
	// branch point.
	// ─────────────────────────────────────────────────────────────────────────
	static void WriteDataTableRow(
		UDataTable* DT,
		const UScriptStruct* RowStruct,
		const FString& RowName,
		const TSharedPtr<FJsonObject>& RowObj,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport)
	{
		FBulkFillFieldWrite RowWrite;
		RowWrite.Path = FString::Printf(TEXT("rows[%s]"), *RowName);
		RowWrite.bOk = false;

		if (!RowObj.IsValid())
		{
			RowWrite.Reason = TEXT("row value is not a JSON object");
			OutReport.FieldWrites.Add(RowWrite);
			OutReport.Errors++;
			return;
		}

		// Allocate + initialise a transient row buffer; walk RowObj fields against
		// the row struct's FProperty list; AddRow with overwrite-aware semantics.
		uint8* RowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
		RowStruct->InitializeStruct(RowData);

		int32 FieldErrors = 0;
		for (const auto& FieldKV : RowObj->Values)
		{
			const FString& FieldName = FieldKV.Key;
			FBulkFillFieldWrite FieldWrite;
			FieldWrite.Path = FString::Printf(TEXT("rows[%s].%s"), *RowName, *FieldName);

			FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldName));
			if (!Prop)
			{
				FieldWrite.Reason = FString::Printf(
					TEXT("row-struct '%s' has no field '%s'"),
					*RowStruct->GetName(), *FieldName);
				FieldWrite.bOk = false;
				OutReport.FieldWrites.Add(FieldWrite);
				// Unknown field = silent-drop candidate when strict mode rejects.
				OutReport.SilentDrops.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}

			// Stringify the JSON value once for ImportText_Direct ingestion.
			// ImportText handles scalar (int/float/bool/FString/FName), struct
			// (parentheses-quoted), array (paren-list), enum (string), and
			// soft-ref (LongPackage.Asset) shapes via the FProperty subtree.
			FString JsonAsString;
			if (FieldKV.Value->Type == EJson::String)
			{
				FieldKV.Value->TryGetString(JsonAsString);
			}
			else
			{
				// Serialize back to a JSON-string form for ImportText. This loses
				// some fidelity vs FJsonObjectConverter::JsonValueToUProperty but
				// matches the row-struct grammar most authors hand-write in CSV.
				TSharedRef<TJsonWriter<TCHAR>> Writer =
					TJsonWriterFactory<TCHAR>::Create(&JsonAsString);
				FJsonSerializer::Serialize(FieldKV.Value.ToSharedRef(), TEXT(""), Writer);
			}
			FieldWrite.ProposedValue = JsonAsString;

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			const TCHAR* ImportResult = Prop->ImportText_Direct(
				*JsonAsString, ValuePtr, nullptr, PPF_None);
			if (ImportResult == nullptr)
			{
				FieldWrite.Reason = FString::Printf(
					TEXT("ImportText_Direct rejected value '%s' for FProperty '%s' (type %s)"),
					*JsonAsString, *FieldName, *Prop->GetCPPType());
				FieldWrite.bOk = false;
				OutReport.FieldWrites.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}

			FieldWrite.bOk = true;
			OutReport.FieldWrites.Add(FieldWrite);
		}

		if (!Spec.bDryRun && FieldErrors == 0)
		{
			DT->AddRow(FName(*RowName), reinterpret_cast<const uint8*>(RowData), RowStruct);
			RowWrite.bOk = true;
		}
		else if (Spec.bDryRun)
		{
			// Dry-run: signal "this row would commit" via bOk=true even though no
			// AddRow happened. Errors==0 only.
			RowWrite.bOk = (FieldErrors == 0);
			if (FieldErrors > 0)
			{
				RowWrite.Reason = FString::Printf(
					TEXT("%d field error(s) in row — see FieldWrites"), FieldErrors);
			}
		}
		else
		{
			RowWrite.Reason = FString::Printf(
				TEXT("%d field error(s) in row — row not committed"), FieldErrors);
		}

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);

		OutReport.FieldWrites.Add(RowWrite);
		if (RowWrite.bOk && !Spec.bDryRun)
		{
			if (!OutReport.WouldModify.Contains(RowName))
			{
				OutReport.WouldModify.Add(RowName);
			}
		}
	}

	// ─────────────────────────────────────────────────────────────────────────
	// fill_kind=DataTableRows — vanilla UDataTable path (any FTableRowBase).
	// SINGLE-TRANSACTION invariant: one FScopedTransaction + one Modify + N AddRow.
	// ─────────────────────────────────────────────────────────────────────────
	static FDryRunReport HandleDataTableRows(const FBulkFillSpec& Spec)
	{
		FString LoadErr;
		UDataTable* DT = ResolveDataTable(Spec.TargetAsset, LoadErr);
		if (!DT)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ui adapter: %s"), *LoadErr));
		}
		const UScriptStruct* RowStruct = DT->RowStruct;
		if (!RowStruct)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ui adapter: DataTable '%s' has no RowStruct"), *Spec.TargetAsset));
		}

		const TSharedPtr<FJsonObject>* RowsObjPtr = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("rows"), RowsObjPtr)
			|| !RowsObjPtr || !RowsObjPtr->IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("ui adapter: spec.tree.rows missing or not an object"));
		}
		const TSharedPtr<FJsonObject>& RowsObj = *RowsObjPtr;

		FDryRunReport Report;

		// SINGLE-TRANSACTION ENVELOPE: process-wide lock + one FScopedTransaction +
		// one Modify() + N AddRow + one MarkPackageDirty. This is the FIX for the
		// 2026-04-25 parallel-burst editor crash signature.
		TSharedPtr<FScopeLock> ProcessLock;
		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			ProcessLock = MakeShared<FScopeLock>(&GUIBulkFillCriticalSection);
			DT->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("UIBulkFill_DataTableRows", "Monolith UI Bulk Fill — DataTable Rows"));
			DT->Modify();
		}

		for (const auto& RowKV : RowsObj->Values)
		{
			const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
			TSharedPtr<FJsonObject> RowObj;
			if (RowKV.Value->TryGetObject(RowObjPtr) && RowObjPtr && RowObjPtr->IsValid())
			{
				RowObj = *RowObjPtr;
			}
			WriteDataTableRow(DT, RowStruct, RowKV.Key, RowObj, Spec, Report);
		}

		// Strict-mode rejects the whole batch.
		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			DT->MarkPackageDirty();
			Report.bWouldApply = true;
		}
		return Report;
	}

#if WITH_COMMONUI
	// ─────────────────────────────────────────────────────────────────────────
	// fill_kind=InputActionDataTable — CommonUI path.
	// Row-struct must be FCommonInputActionDataBase (verified
	// CommonUI/Public/CommonUITypes.h:100 — `: public FTableRowBase`).
	// SINGLE-TRANSACTION invariant: closes the 2026-04-25 parallel-burst crash.
	// ─────────────────────────────────────────────────────────────────────────
	static FDryRunReport HandleInputActionDataTable(const FBulkFillSpec& Spec)
	{
		FString LoadErr;
		UDataTable* DT = ResolveDataTable(Spec.TargetAsset, LoadErr);
		if (!DT)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ui adapter: %s"), *LoadErr));
		}

		const UScriptStruct* InputActionStruct = FCommonInputActionDataBase::StaticStruct();
		if (!InputActionStruct)
		{
			return MakeResolveFailureReport(
				TEXT("ui adapter: FCommonInputActionDataBase::StaticStruct returned null — CommonUI module load issue"));
		}
		if (DT->RowStruct != InputActionStruct)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ui adapter: DT RowStruct is %s, expected FCommonInputActionDataBase"),
				DT->RowStruct ? *DT->RowStruct->GetName() : TEXT("(null)")));
		}

		const TSharedPtr<FJsonObject>* RowsObjPtr = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("rows"), RowsObjPtr)
			|| !RowsObjPtr || !RowsObjPtr->IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("ui adapter: spec.tree.rows missing or not an object"));
		}
		const TSharedPtr<FJsonObject>& RowsObj = *RowsObjPtr;

		FDryRunReport Report;

		// SINGLE-TRANSACTION ENVELOPE — this is THE 2026-04-25 crash fix.
		// ONE process-wide lock + ONE FScopedTransaction + ONE Modify + N AddRow +
		// ONE MarkPackageDirty wrapping the whole 40-row batch.
		TSharedPtr<FScopeLock> ProcessLock;
		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			ProcessLock = MakeShared<FScopeLock>(&GUIBulkFillCriticalSection);
			DT->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("UIBulkFill_InputActionDT",
					"Monolith UI Bulk Fill — Input Action DT (single transaction)"));
			DT->Modify();
		}

		for (const auto& RowKV : RowsObj->Values)
		{
			const TSharedPtr<FJsonObject>* RowObjPtr = nullptr;
			TSharedPtr<FJsonObject> RowObj;
			if (RowKV.Value->TryGetObject(RowObjPtr) && RowObjPtr && RowObjPtr->IsValid())
			{
				RowObj = *RowObjPtr;
			}
			WriteDataTableRow(DT, InputActionStruct, RowKV.Key, RowObj, Spec, Report);
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			DT->MarkPackageDirty();
			Report.bWouldApply = true;
		}
		return Report;
	}
#endif // WITH_COMMONUI

	// ─────────────────────────────────────────────────────────────────────────
	// fill_kind=WidgetProperties — vanilla UMG widget property writes.
	// Walks "properties": { name: value, ... } against the resolved widget's
	// UClass via ImportText_Direct. Uses ONE FScopedTransaction wrapping the
	// whole bag (matches the DT row-batch invariant).
	// ─────────────────────────────────────────────────────────────────────────
	static FDryRunReport HandleWidgetProperties(const FBulkFillSpec& Spec)
	{
		FString WidgetName;
		Spec.Tree->TryGetStringField(TEXT("widget_name"), WidgetName);
		if (WidgetName.IsEmpty())
		{
			return MakeResolveFailureReport(
				TEXT("ui adapter: WidgetProperties fill_kind requires 'widget_name'"));
		}

		FString ResolveErr;
		UWidget* Widget = ResolveWidgetInWBP(Spec.TargetAsset, WidgetName, ResolveErr);
		if (!Widget)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("ui adapter: %s"), *ResolveErr));
		}

		const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("properties"), PropsObjPtr)
			|| !PropsObjPtr || !PropsObjPtr->IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("ui adapter: spec.tree.properties missing or not an object"));
		}
		const TSharedPtr<FJsonObject>& PropsObj = *PropsObjPtr;

		FDryRunReport Report;

		TSharedPtr<FScopeLock> ProcessLock;
		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			ProcessLock = MakeShared<FScopeLock>(&GUIBulkFillCriticalSection);
			Widget->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("UIBulkFill_WidgetProperties",
					"Monolith UI Bulk Fill — Widget Properties"));
			Widget->Modify();
		}

		for (const auto& KV : PropsObj->Values)
		{
			FBulkFillFieldWrite Write;
			Write.Path = FString::Printf(TEXT("%s.%s"), *WidgetName, *KV.Key);
			FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*KV.Key));
			if (!Prop)
			{
				Write.Reason = FString::Printf(
					TEXT("widget '%s' (class %s) has no UPROPERTY '%s'"),
					*WidgetName, *Widget->GetClass()->GetName(), *KV.Key);
				Write.bOk = false;
				Report.FieldWrites.Add(Write);
				Report.SilentDrops.Add(Write);
				Report.Errors++;
				continue;
			}

			FString JsonAsString;
			if (KV.Value->Type == EJson::String)
			{
				KV.Value->TryGetString(JsonAsString);
			}
			else
			{
				TSharedRef<TJsonWriter<TCHAR>> Writer =
					TJsonWriterFactory<TCHAR>::Create(&JsonAsString);
				FJsonSerializer::Serialize(KV.Value.ToSharedRef(), TEXT(""), Writer);
			}
			Write.ProposedValue = JsonAsString;

			if (Spec.bDryRun)
			{
				Write.bOk = true;
				Report.FieldWrites.Add(Write);
				continue;
			}

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
			const TCHAR* ImportRes = Prop->ImportText_Direct(
				*JsonAsString, ValuePtr, Widget, PPF_None);
			if (ImportRes == nullptr)
			{
				Write.Reason = FString::Printf(
					TEXT("ImportText_Direct rejected value for property '%s' (type %s)"),
					*KV.Key, *Prop->GetCPPType());
				Write.bOk = false;
				Report.FieldWrites.Add(Write);
				Report.Errors++;
				continue;
			}
			Write.bOk = true;
			Report.FieldWrites.Add(Write);
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			Widget->MarkPackageDirty();
			Report.bWouldApply = true;
			Report.WouldModify.Add(WidgetName);
		}
		return Report;
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Describe builders
	// ─────────────────────────────────────────────────────────────────────────

	// Walk a UClass for slot-prop UPROPERTYs and emit one descriptor per. The
	// slot classes (UCanvasPanelSlot, UVerticalBoxSlot, etc.) carry per-parent
	// properties that the editor surfaces as the "Slot" panel — Anchors+Offsets
	// for Canvas, Size+H/VAlign+Padding for VerticalBox, etc.
	static FSchemaDescriptor BuildSlotPropertyDescriptor(UClass* SlotClass)
	{
		FSchemaDescriptor Desc;
		if (!SlotClass)
		{
			Desc.FieldPath = TEXT("(slot)");
			Desc.TypeName = TEXT("error");
			Desc.ImportTextForm = TEXT("parent panel has no slot class");
			return Desc;
		}
		Desc.FieldPath = SlotClass->GetName();
		Desc.TypeName = FString::Printf(TEXT("UPanelSlot<%s>"), *SlotClass->GetName());
		Desc.ImportTextForm = TEXT("(slot-prop tree below — per-parent class)");

		for (TFieldIterator<FProperty> It(SlotClass); It; ++It)
		{
			FProperty* P = *It;
			if (!P) continue;
			// Skip parent-class fields inherited from UPanelSlot (Parent / Content)
			// — those are bookkeeping, not editor-surfaced "Slot" props.
			if (P->GetOwnerClass() && P->GetOwnerClass()->GetName() == TEXT("PanelSlot"))
			{
				continue;
			}
			FSchemaDescriptor Child;
			Child.FieldPath = P->GetName();
			Child.TypeName = P->GetCPPType();

			// Numeric meta clamps (matches Phase 0 design Q3).
			const FString UIMin   = P->HasMetaData(TEXT("UIMin"))   ? P->GetMetaData(TEXT("UIMin"))   : TEXT("");
			const FString UIMax   = P->HasMetaData(TEXT("UIMax"))   ? P->GetMetaData(TEXT("UIMax"))   : TEXT("");
			const FString ClampMin = P->HasMetaData(TEXT("ClampMin")) ? P->GetMetaData(TEXT("ClampMin")) : TEXT("");
			const FString ClampMax = P->HasMetaData(TEXT("ClampMax")) ? P->GetMetaData(TEXT("ClampMax")) : TEXT("");
			if (!UIMin.IsEmpty()   || !ClampMin.IsEmpty())
				Child.RangeMin = FCString::Atof(*(UIMin.IsEmpty() ? ClampMin : UIMin));
			if (!UIMax.IsEmpty()   || !ClampMax.IsEmpty())
				Child.RangeMax = FCString::Atof(*(UIMax.IsEmpty() ? ClampMax : UIMax));

			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(P))
			{
				if (UEnum* E = EnumProp->GetEnum())
				{
					for (int32 i = 0; i < E->NumEnums() - 1; ++i)
					{
						Child.EnumValues.Add(E->GetNameStringByIndex(i));
					}
				}
			}
			Child.ImportTextForm = FString::Printf(TEXT("(see %s ImportText grammar)"), *P->GetCPPType());
			Desc.Children.Add(Child);
		}
		return Desc;
	}

	// Top-level describe tree when caller passes empty / unknown target.
	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("ui");
		Root.TypeName = TEXT("Namespace<UMG + CommonUI>");
		Root.ImportTextForm = TEXT(
			"target=<DT path | WBP path with `|widget_name=X[|kind=slot|widget]`>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample, const TCHAR* Note)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			// FSchemaDescriptor carries no Reason field; documentation rides in a
			// child descriptor so the schema-walk JSON shape stays uniform.
			FSchemaDescriptor Doc;
			Doc.FieldPath = TEXT("(note)");
			Doc.TypeName = TEXT("doc");
			Doc.ImportTextForm = Note;
			K.Children.Add(Doc);
			Root.Children.Add(K);
		};

		AddKind(
			TEXT("DataTableRows"),
			TEXT("{\"fill_kind\":\"DataTableRows\",\"rows\":{\"R1\":{\"field\":1}}}"),
			TEXT("Vanilla UDataTable row-batch. Any FTableRowBase row-struct. SINGLE TRANSACTION."));
#if WITH_COMMONUI
		AddKind(
			TEXT("InputActionDataTable"),
			TEXT("{\"fill_kind\":\"InputActionDataTable\",\"rows\":{\"Inv_Open\":{\"DefaultDisplayName\":\"Open Inventory\"}}}"),
			TEXT("CommonUI input-action DT (FCommonInputActionDataBase). 40-row SINGLE TRANSACTION — fixes 2026-04-25 parallel-burst crash."));
#else
		AddKind(
			TEXT("InputActionDataTable"),
			TEXT("(unavailable — WITH_COMMONUI=0)"),
			TEXT("CommonUI not present in this build."));
#endif
		AddKind(
			TEXT("WidgetProperties"),
			TEXT("{\"fill_kind\":\"WidgetProperties\",\"widget_name\":\"T_Title\",\"properties\":{\"Text\":\"Hi\"}}"),
			TEXT("UMG widget UPROPERTY writes. Resolves widget inside WBP WidgetTree."));
		return Root;
	}
} // namespace MonolithUIBulkFillInternal

// ---------------------------------------------------------------------------
// FBulkFillAdapter — invoked from bulk_fill.apply for target_namespace="ui"
// ---------------------------------------------------------------------------
FDryRunReport FMonolithUIBulkFillAdapter::UIBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithUIBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("ui adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"ui adapter: spec.tree.fill_kind required — one of "
			"'DataTableRows', 'InputActionDataTable', 'WidgetProperties'"));
	}

	// VANILLA UMG PATHS — gate-free. Run regardless of WITH_COMMONUI.
	if (FillKind == TEXT("DataTableRows"))
	{
		return HandleDataTableRows(Spec);
	}
	if (FillKind == TEXT("WidgetProperties"))
	{
		return HandleWidgetProperties(Spec);
	}

	// COMMONUI-SPECIFIC PATHS — gated `#if WITH_COMMONUI`.
	if (FillKind == TEXT("InputActionDataTable"))
	{
#if WITH_COMMONUI
		return HandleInputActionDataTable(Spec);
#else
		return MakeResolveFailureReport(TEXT(
			"ui adapter: InputActionDataTable fill_kind requires CommonUI "
			"(WITH_COMMONUI=0 in this build). Vanilla UMG paths (DataTableRows, "
			"WidgetProperties) remain available."));
#endif
	}

	return MakeResolveFailureReport(FString::Printf(
		TEXT("ui adapter: unknown fill_kind '%s' (supported: DataTableRows, "
		     "InputActionDataTable, WidgetProperties)"),
		*FillKind));
}

// ---------------------------------------------------------------------------
// FDescribeAdapter — invoked from describe.schema for target_namespace="ui"
// ---------------------------------------------------------------------------
//
// TargetAsset encoding (pipe-delimited compound key):
//   "/Game/UI/WBP_Foo|widget_name=MyButton"                → slot-prop describe
//   "/Game/UI/WBP_Foo|widget_name=MyButton|kind=widget"    → widget-prop describe
//   "/Game/UI/DT_Foo"                                      → DT row-struct describe
//   ""                                                     → top-level describe
//
// The pipe-delimited key keeps the describe-adapter API stable (single FString
// param matches the Phase 0 FDescribeAdapter signature) while threading the
// child-widget context needed for the slot-prop scoping.
//
FSchemaDescriptor FMonolithUIBulkFillAdapter::UIDescribe(const FString& TargetAsset)
{
	using namespace MonolithUIBulkFillInternal;

	if (TargetAsset.IsEmpty())
	{
		return BuildTopLevelDescribe();
	}

	// Parse compound key.
	TArray<FString> Tokens;
	TargetAsset.ParseIntoArray(Tokens, TEXT("|"), /*bCullEmpty*/ true);
	const FString& AssetPath = Tokens.Num() > 0 ? Tokens[0] : TargetAsset;

	FString WidgetName;
	FString DescribeKind = TEXT("slot"); // default for compound-key form
	for (int32 i = 1; i < Tokens.Num(); ++i)
	{
		const FString& T = Tokens[i];
		if (T.StartsWith(TEXT("widget_name="), ESearchCase::IgnoreCase))
		{
			WidgetName = T.Mid(12);
		}
		else if (T.StartsWith(TEXT("widget="), ESearchCase::IgnoreCase))
		{
			WidgetName = T.Mid(7);
		}
		else if (T.StartsWith(TEXT("kind="), ESearchCase::IgnoreCase))
		{
			DescribeKind = T.Mid(5).ToLower();
		}
	}

	// If no widget context → describe the DT row-struct OR fall back to top-level.
	if (WidgetName.IsEmpty())
	{
		FString LoadErr;
		UDataTable* DT = ResolveDataTable(AssetPath, LoadErr);
		if (DT && DT->RowStruct)
		{
			FSchemaDescriptor Desc;
			Desc.FieldPath = AssetPath;
			Desc.TypeName = FString::Printf(TEXT("UDataTable<%s>"), *DT->RowStruct->GetName());
			Desc.ImportTextForm = TEXT("(rows: {RowName: {field: value, ...}})");
			// Emit one descriptor per row-struct field.
			for (TFieldIterator<FProperty> It(DT->RowStruct); It; ++It)
			{
				FProperty* P = *It;
				if (!P) continue;
				FSchemaDescriptor Child;
				Child.FieldPath = P->GetName();
				Child.TypeName = P->GetCPPType();
				if (FEnumProperty* EnumP = CastField<FEnumProperty>(P))
				{
					if (UEnum* E = EnumP->GetEnum())
					{
						for (int32 i = 0; i < E->NumEnums() - 1; ++i)
						{
							Child.EnumValues.Add(E->GetNameStringByIndex(i));
						}
					}
				}
				Desc.Children.Add(Child);
			}
			return Desc;
		}
		// No DT and no widget context → top-level.
		return BuildTopLevelDescribe();
	}

	// Widget context present → resolve to UWidget then branch on DescribeKind.
	FString ResolveErr;
	UWidget* Widget = ResolveWidgetInWBP(AssetPath, WidgetName, ResolveErr);
	if (!Widget)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(adapter)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(TEXT("ui describe: %s"), *ResolveErr);
		return Err;
	}

	if (DescribeKind == TEXT("widget"))
	{
		// Widget-property describe: emit one descriptor per UPROPERTY on the
		// resolved widget's UClass. NOT the panel's slot props — those are
		// kind=slot. This complements (does not replace) the existing
		// dump_property_allowlist action — we surface the type tags here too.
		FSchemaDescriptor Desc;
		Desc.FieldPath = WidgetName;
		Desc.TypeName = FString::Printf(TEXT("UWidget<%s>"), *Widget->GetClass()->GetName());
		Desc.ImportTextForm = TEXT("(widget UPROPERTY tree)");
		for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
		{
			FProperty* P = *It;
			if (!P) continue;
			FSchemaDescriptor Child;
			Child.FieldPath = P->GetName();
			Child.TypeName = P->GetCPPType();
			if (FEnumProperty* EnumP = CastField<FEnumProperty>(P))
			{
				if (UEnum* E = EnumP->GetEnum())
				{
					for (int32 i = 0; i < E->NumEnums() - 1; ++i)
					{
						Child.EnumValues.Add(E->GetNameStringByIndex(i));
					}
				}
			}
			Desc.Children.Add(Child);
		}
		return Desc;
	}

	// kind=slot (DEFAULT for compound key) — slot-prop describe scoped to
	// PARENT PANEL CLASS. Slot props are per-parent in UE 5.7, NOT per-widget.
	// This is the worst UI ergonomic pain (design B.11) — fixed here.
	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(slot)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(
			TEXT("widget '%s' has no UPanelSlot (root widgets / orphaned widgets carry no slot)"),
			*WidgetName);
		return Err;
	}
	UClass* SlotClass = Slot->GetClass();
	FSchemaDescriptor SlotDesc = BuildSlotPropertyDescriptor(SlotClass);

	// Attach context so the consumer sees "this is the slot tree FOR widget X
	// when its parent is Y panel".
	UPanelWidget* ParentPanel = Slot->Parent;
	FSchemaDescriptor Root;
	Root.FieldPath = FString::Printf(
		TEXT("%s.slot (parent=%s)"),
		*WidgetName,
		ParentPanel ? *ParentPanel->GetClass()->GetName() : TEXT("(unparented)"));
	Root.TypeName = SlotDesc.TypeName;
	Root.ImportTextForm = SlotDesc.ImportTextForm;
	Root.Children = MoveTemp(SlotDesc.Children);
	return Root;
}

// ---------------------------------------------------------------------------
// Registration entry-points (called from FMonolithUIModule::StartupModule
// / ShutdownModule).
//
// **H5 invariant**: Register() runs unconditionally — the body switches on
// fill_kind / WITH_COMMONUI inside the dispatchers, NOT around the registration.
// This guarantees `monolith_discover("ui")` returns the same row in dev and
// release builds.
//
// **M5 invariant**: CommonUI gating happens INSIDE UIBulkFill (per fill_kind),
// NOT around RegisterAdapter. Vanilla UMG paths (DataTableRows, WidgetProperties,
// slot-prop describe) work without CommonUI.
// ---------------------------------------------------------------------------
void FMonolithUIBulkFillAdapter::Register()
{
	// FBulkFillAdapter / FDescribeAdapter are TFunction<>; pass function pointers
	// directly. CreateStatic is delegate-class API and would not compile here
	// (Phase 1 drift discovery, preserved through Phase 2/3/4).
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("ui"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithUIBulkFillAdapter::UIBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithUIBulkFillAdapter::UIDescribe));
}

void FMonolithUIBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("ui"));
}

#undef LOCTEXT_NAMESPACE
