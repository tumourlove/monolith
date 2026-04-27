#include "MonolithLevelSequenceIndexer.h"
#include "MonolithIndexDatabase.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneBindingReferences.h"
#include "Bindings/MovieSceneCustomBinding.h"
#include "Bindings/MovieSceneSpawnableBinding.h"
#include "Bindings/MovieSceneReplaceableBinding.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Sections/MovieSceneEventRepeaterSection.h"
#include "Channels/MovieSceneEvent.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ─────────────────────────────────────────────────────────────
// Local helpers
// ─────────────────────────────────────────────────────────────

namespace
{
	/** Format a pin's data type as a human-readable string via the K2 schema. */
	FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		return UEdGraphSchema_K2::TypeToText(PinType).ToString();
	}

	/** Convert an FBPVariableDescription's type to a human-readable string. */
	FString VarTypeToString(const FEdGraphPinType& PinType)
	{
		return PinTypeToString(PinType);
	}

	/**
	 * Bind a string to a prepared-statement parameter, mapping empty strings to
	 * SQL NULL. The single-argument SetBindingValueByIndex(int32) overload binds
	 * NULL — see SQLitePreparedStatement.h:197-198.
	 */
	void BindNullableString(FSQLitePreparedStatement& Stmt, int32 Index, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			Stmt.SetBindingValueByIndex(Index);
		}
		else
		{
			Stmt.SetBindingValueByIndex(Index, Value);
		}
	}

	/** Bind a positive int64 as the column value, or NULL when value <= 0. */
	void BindOptionalRowId(FSQLitePreparedStatement& Stmt, int32 Index, int64 RowId)
	{
		if (RowId > 0)
		{
			Stmt.SetBindingValueByIndex(Index, RowId);
		}
		else
		{
			Stmt.SetBindingValueByIndex(Index);
		}
	}

	/** Run a DELETE/UPDATE prepared statement that takes a single int64 parameter. */
	void ExecWithInt64(FSQLiteDatabase* RawDB, const TCHAR* Sql, int64 Param1)
	{
		if (!RawDB) return;
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, Sql))
		{
			Stmt.SetBindingValueByIndex(1, Param1);
			Stmt.Execute();
		}
		Stmt.Destroy();
	}

	/**
	 * Serialize a list of UEdGraphPin* into a JSON array of {name, type} objects.
	 * Filters out hidden pins, exec pins, and the special exec-flow pins.
	 * Only includes pins matching the requested direction.
	 */
	FString BuildSignatureJson(const TArray<UEdGraphPin*>& Pins, EEdGraphPinDirection WantDir)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&Out);
		Writer->WriteArrayStart();
		for (UEdGraphPin* Pin : Pins)
		{
			if (!Pin) continue;
			if (Pin->bHidden) continue;
			if (Pin->Direction != WantDir) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->PinName == UEdGraphSchema_K2::PN_Then ||
			    Pin->PinName == UEdGraphSchema_K2::PN_Execute ||
			    Pin->PinName == UEdGraphSchema_K2::PN_Self)
			{
				continue;
			}

			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Pin->PinName.ToString());
			Writer->WriteValue(TEXT("type"), PinTypeToString(Pin->PinType));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->Close();
		return Out;
	}

	/** Extract input-parameter signature from a user function graph (find UK2Node_FunctionEntry). */
	FString ExtractUserFunctionSignature(UEdGraph* Graph)
	{
		if (!Graph) return TEXT("[]");
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
			{
				// On FunctionEntry, the function's input parameters are the OUTPUT pins
				// (they flow out from the entry node into the graph body).
				return BuildSignatureJson(Entry->Pins, EGPD_Output);
			}
		}
		return TEXT("[]");
	}

	/** Extract input-parameter signature from a CustomEvent node. */
	FString ExtractCustomEventSignature(UK2Node_CustomEvent* CustomEvent)
	{
		if (!CustomEvent) return TEXT("[]");
		// On CustomEvent, parameters are also the OUTPUT pins (same flow direction as FunctionEntry).
		return BuildSignatureJson(CustomEvent->Pins, EGPD_Output);
	}

	/** Find existing director row id for the given LS path; returns -1 if none. */
	int64 SelectExistingDirectorId(FSQLiteDatabase* RawDB, const FString& LsPath)
	{
		if (!RawDB) return -1;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id FROM level_sequence_directors WHERE ls_path = ?"), ESQLitePreparedStatementFlags::Persistent))
		{
			return -1;
		}
		Stmt.SetBindingValueByIndex(1, LsPath);
		int64 FoundId = -1;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, FoundId);
		}
		Stmt.Destroy();
		return FoundId;
	}

	/**
	 * Find existing director row id AND its ls_asset_id for the given LS path.
	 * The ls_asset_id from a previous reindex pass may differ from the current
	 * AssetId (the core asset row gets a fresh autoincrement id every full
	 * reindex), so we need both to clean child rows that key off ls_asset_id.
	 */
	void SelectExistingDirectorIdAndAssetId(FSQLiteDatabase* RawDB, const FString& LsPath, int64& OutDirId, int64& OutAssetId)
	{
		OutDirId = -1;
		OutAssetId = -1;
		if (!RawDB) return;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT("SELECT id, ls_asset_id FROM level_sequence_directors WHERE ls_path = ?"), ESQLitePreparedStatementFlags::Persistent))
		{
			return;
		}
		Stmt.SetBindingValueByIndex(1, LsPath);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, OutDirId);
			Stmt.GetColumnValueByIndex(1, OutAssetId);
		}
		Stmt.Destroy();
	}

	/**
	 * Get the function-name an FMovieSceneEvent fires.
	 * Per UE 5.7 source (MovieSceneEventSectionBase.cpp::OnPostCompile):
	 *   - Ptrs.Function is the canonical post-compile UFunction* — prefer it.
	 *   - CompiledFunctionName is editor-only, set during OnPostCompile and
	 *     normally cleared right after Ptrs.Function is wired up.
	 * Returns empty string if neither is populated (event not yet bound).
	 */
	FString GetEventFunctionName(const FMovieSceneEvent& Event)
	{
		if (Event.Ptrs.Function)
		{
			return Event.Ptrs.Function->GetName();
		}
#if WITH_EDITORONLY_DATA
		if (!Event.CompiledFunctionName.IsNone())
		{
			return Event.CompiledFunctionName.ToString();
		}
#endif
		return FString();
	}

	/** Insert one row into level_sequence_event_bindings via a prepared statement. */
	void InsertEventBindingRow(
		FSQLiteDatabase* RawDB,
		int64 LsAssetId,
		const FString& BindingGuidStr,
		const FString& BindingName,
		const FString& BindingKind,
		const FString& BoundClass,
		const FString& SectionKind,
		const FString& FiresFunctionName,
		int64 FiresFunctionId)
	{
		if (!RawDB) return;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO level_sequence_event_bindings "
			"(ls_asset_id, binding_guid, binding_name, binding_kind, bound_class, "
			" section_kind, fires_function_name, fires_function_id) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)")))
		{
			return;
		}

		Stmt.SetBindingValueByIndex(1, LsAssetId);
		BindNullableString(Stmt, 2, BindingGuidStr);
		BindNullableString(Stmt, 3, BindingName);
		BindNullableString(Stmt, 4, BindingKind);
		BindNullableString(Stmt, 5, BoundClass);
		// section_kind is NOT NULL — caller always provides it ('trigger' / 'repeater').
		Stmt.SetBindingValueByIndex(6, SectionKind);
		BindNullableString(Stmt, 7, FiresFunctionName);
		BindOptionalRowId(Stmt, 8, FiresFunctionId);

		Stmt.Execute();
		Stmt.Destroy();
	}

	/**
	 * Walk a single event track's sections and insert one row per FMovieSceneEvent
	 * (Trigger sections may contain multiple timed events; Repeater sections have one).
	 */
	void ProcessEventTrack(
		const UMovieSceneEventTrack* EvTrack,
		FSQLiteDatabase* RawDB,
		int64 LsAssetId,
		const FString& BindingGuidStr,
		const FString& BindingName,
		const FString& BindingKind,
		const FString& BoundClass,
		const TMap<FName, int64>& FuncNameToId)
	{
		if (!EvTrack) return;

		auto ResolveFuncId = [&FuncNameToId](const FString& Name) -> int64
		{
			if (Name.IsEmpty()) return -1;
			const int64* Found = FuncNameToId.Find(FName(*Name));
			return Found ? *Found : -1;
		};

		for (UMovieSceneSection* Section : EvTrack->GetAllSections())
		{
			if (!Section) continue;

			if (const UMovieSceneEventTriggerSection* Trigger = Cast<UMovieSceneEventTriggerSection>(Section))
			{
				const FMovieSceneEventChannel& Channel = Trigger->EventChannel;
				TArrayView<const FMovieSceneEvent> Events = Channel.GetData().GetValues();
				for (const FMovieSceneEvent& Ev : Events)
				{
					const FString FuncName = GetEventFunctionName(Ev);
					InsertEventBindingRow(RawDB, LsAssetId, BindingGuidStr, BindingName, BindingKind, BoundClass,
						TEXT("trigger"), FuncName, ResolveFuncId(FuncName));
				}
				if (Events.Num() == 0)
				{
					// Empty trigger section — record one stub row so cinematics with no
					// keys yet still show up in inspections.
					InsertEventBindingRow(RawDB, LsAssetId, BindingGuidStr, BindingName, BindingKind, BoundClass,
						TEXT("trigger"), FString(), -1);
				}
			}
			else if (const UMovieSceneEventRepeaterSection* Repeater = Cast<UMovieSceneEventRepeaterSection>(Section))
			{
				const FString FuncName = GetEventFunctionName(Repeater->Event);
				InsertEventBindingRow(RawDB, LsAssetId, BindingGuidStr, BindingName, BindingKind, BoundClass,
					TEXT("repeater"), FuncName, ResolveFuncId(FuncName));
			}
		}
	}

	/**
	 * Classify a binding GUID into (name, kind, bound_class, custom_binding_class, custom_binding_pretty).
	 *
	 * UE 5.7 introduced UMovieSceneCustomBinding (in BindingReferences) which sits ALONGSIDE
	 * the legacy FMovieScenePossessable / FMovieSceneSpawnable structures. The migration path
	 * (see ULevelSequence::ConvertOldSpawnables) registers modern Spawnables AS Possessables
	 * inside UMovieScene while attaching their real spawnable identity to BindingReferences.
	 * That means FindPossessable() returns non-null for them, and they would be mis-classified
	 * as "possessable" if we didn't also consult Sequence->GetBindingReferences().
	 *
	 * Output meaning:
	 *   OutKind             — editor-facing kind: possessable / spawnable / replaceable / custom / unknown
	 *   OutClass            — class of the bound object (preferred from CustomBinding when present)
	 *   OutCustomClass      — exact UCLASS name of the custom binding (e.g. MovieSceneSpawnableActorBinding)
	 *   OutCustomPretty     — human-readable type label from GetBindingTypePrettyName()
	 */
	void ClassifyBinding(
		UMovieSceneSequence* Seq, UMovieScene* MS, const FGuid& Guid, int32 BindingIndex,
		FString& OutName, FString& OutKind, FString& OutClass,
		FString& OutCustomClass, FString& OutCustomPretty)
	{
		OutName.Reset();
		OutKind.Reset();
		OutClass.Reset();
		OutCustomClass.Reset();
		OutCustomPretty.Reset();
		if (!MS) return;

		FMovieScenePossessable* Possessable = MS->FindPossessable(Guid);
		FMovieSceneSpawnable*   LegacySpawn = MS->FindSpawnable(Guid);

		if (Possessable)
		{
			OutName = Possessable->GetName();
#if WITH_EDITORONLY_DATA
			if (const UClass* Cls = Possessable->GetPossessedObjectClass())
			{
				OutClass = Cls->GetName();
			}
#endif
		}
		else if (LegacySpawn)
		{
			OutName = LegacySpawn->GetName();
			OutKind = TEXT("spawnable");
			if (const UObject* Tmpl = LegacySpawn->GetObjectTemplate())
			{
				OutClass = Tmpl->GetClass()->GetName();
			}
		}

		const UMovieSceneCustomBinding* Custom = nullptr;
		if (Seq)
		{
			if (const FMovieSceneBindingReferences* Refs = Seq->GetBindingReferences())
			{
				Custom = Refs->GetCustomBinding(Guid, BindingIndex);
			}
		}

		if (Custom)
		{
			OutCustomClass  = Custom->GetClass()->GetName();
			OutCustomPretty = Custom->GetBindingTypePrettyName().ToString();

			if (UClass* BoundCls = Custom->GetBoundObjectClass())
			{
				// CustomBinding's bound class is more authoritative than the
				// possessable upgrade-stub class — prefer it when both exist.
				OutClass = BoundCls->GetName();
			}

			if (Custom->IsA<UMovieSceneSpawnableBindingBase>())
			{
				OutKind = TEXT("spawnable");
			}
			else if (Custom->IsA<UMovieSceneReplaceableBindingBase>())
			{
				OutKind = TEXT("replaceable");
			}
			else
			{
				OutKind = TEXT("custom");
			}
		}
		else if (OutKind.IsEmpty())
		{
			OutKind = Possessable ? TEXT("possessable") : TEXT("unknown");
		}
	}

	/** Backward-compatible 3-out wrapper around ClassifyBinding (drops custom-binding info). */
	void ResolveBinding(UMovieSceneSequence* Seq, UMovieScene* MS, const FGuid& Guid,
		FString& OutName, FString& OutKind, FString& OutClass)
	{
		FString CustomClass, CustomPretty;
		ClassifyBinding(Seq, MS, Guid, 0, OutName, OutKind, OutClass, CustomClass, CustomPretty);
	}

	/** Insert one row into level_sequence_bindings via a prepared statement. */
	void InsertBindingRow(
		FSQLiteDatabase* RawDB,
		int64 LsAssetId,
		const FString& BindingGuidStr,
		int32 BindingIndex,
		const FString& Name,
		const FString& Kind,
		const FString& BoundClass,
		const FString& CustomBindingClass,
		const FString& CustomBindingPretty,
		int32 TrackCount)
	{
		if (!RawDB) return;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO level_sequence_bindings "
			"(ls_asset_id, binding_guid, binding_index, name, kind, bound_class, "
			" custom_binding_class, custom_binding_pretty, track_count) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")))
		{
			return;
		}

		Stmt.SetBindingValueByIndex(1, LsAssetId);
		Stmt.SetBindingValueByIndex(2, BindingGuidStr);
		Stmt.SetBindingValueByIndex(3, BindingIndex);
		BindNullableString(Stmt, 4, Name);
		// kind is NOT NULL — ClassifyBinding always sets it (worst case 'unknown').
		Stmt.SetBindingValueByIndex(5, Kind);
		BindNullableString(Stmt, 6, BoundClass);
		BindNullableString(Stmt, 7, CustomBindingClass);
		BindNullableString(Stmt, 8, CustomBindingPretty);
		Stmt.SetBindingValueByIndex(9, TrackCount);

		Stmt.Execute();
		Stmt.Destroy();
	}
}

// ─────────────────────────────────────────────────────────────
// IMonolithIndexer overrides
// ─────────────────────────────────────────────────────────────

TArray<FString> FLevelSequenceIndexer::GetSupportedClasses() const
{
	return { TEXT("LevelSequence") };
}

void FLevelSequenceIndexer::EnsureTablesExist(FMonolithIndexDatabase& DB)
{
	if (bTablesCreated) return;

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return;

	// NB: ls_asset_id is intentionally NOT a FOREIGN KEY on assets(id). The core
	// database's ResetDatabase() (called by force=true reindex) wipes built-in
	// tables (assets, nodes, etc.) without knowing about our custom tables. A
	// FK from us to assets makes that DELETE fail and aborts the whole reindex.
	// Pattern borrowed from MonolithAI/FAIIndexer (its ai_assets is also
	// FK-free against core assets for the same reason).
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS level_sequence_directors ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  ls_asset_id INTEGER NOT NULL,"
		"  ls_path TEXT NOT NULL UNIQUE,"
		"  director_bp_name TEXT NOT NULL,"
		"  function_count INTEGER NOT NULL,"
		"  variable_count INTEGER NOT NULL"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_path ON level_sequence_directors(ls_path)"));

	// kind classifies the origin of each function declared on this Director:
	//   'user'               — UEdGraph in DirBP->FunctionGraphs (you-defined function)
	//   'custom_event'       — UK2Node_CustomEvent inside DirBP->UbergraphPages
	//   'sequencer_endpoint' — UE-generated UFunction backing a Sequencer "Quick
	//                          Bind" / "Create New Endpoint" event-track entry
	//                          (name pattern: SequenceEvent__ENTRYPOINT<DirBP>_N)
	// Inherited base-class functions and compiler-generated ExecuteUbergraph*
	// dispatchers are NOT recorded — same convention as MonolithBlueprint's
	// blueprint_query.get_functions (own-functions only).
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS level_sequence_director_functions ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  director_id INTEGER NOT NULL,"
		"  name TEXT NOT NULL,"
		"  kind TEXT NOT NULL,"
		"  signature_json TEXT,"
		"  FOREIGN KEY (director_id) REFERENCES level_sequence_directors(id)"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_func_dir ON level_sequence_director_functions(director_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_func_name ON level_sequence_director_functions(name)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_func_kind ON level_sequence_director_functions(kind)"));

	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS level_sequence_director_variables ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  director_id INTEGER NOT NULL,"
		"  name TEXT NOT NULL,"
		"  type TEXT NOT NULL,"
		"  FOREIGN KEY (director_id) REFERENCES level_sequence_directors(id)"
		")"
	));

	// ls_asset_id deliberately FK-free; see comment above on level_sequence_directors.
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS level_sequence_event_bindings ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  ls_asset_id INTEGER NOT NULL,"
		"  binding_guid TEXT,"
		"  binding_name TEXT,"
		"  binding_kind TEXT,"
		"  bound_class TEXT,"
		"  section_kind TEXT NOT NULL,"
		"  fires_function_name TEXT,"
		"  fires_function_id INTEGER,"
		"  FOREIGN KEY (fires_function_id) REFERENCES level_sequence_director_functions(id)"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_eb_ls ON level_sequence_event_bindings(ls_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_eb_func ON level_sequence_event_bindings(fires_function_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_lsdir_eb_funcname ON level_sequence_event_bindings(fires_function_name)"));

	// Per-binding table — one row per (Guid, BindingIndex) regardless of whether
	// the binding has event tracks. Captures the full UE 5.7 picture including
	// modern Spawnables backed by UMovieSceneCustomBinding (which UMovieScene
	// also registers as Possessables for tracks; the custom binding identity
	// lives on UMovieSceneSequence::GetBindingReferences()).
	//
	// kind ∈ {possessable, spawnable, replaceable, custom, unknown}
	//   spawnable    — legacy FMovieSceneSpawnable OR custom UMovieSceneSpawnableBindingBase
	//   replaceable  — custom UMovieSceneReplaceableBindingBase
	//   custom       — any other UMovieSceneCustomBinding subclass
	//   possessable  — plain possessable with no custom binding
	//
	// custom_binding_class is the exact UCLASS name (e.g. MovieSceneSpawnableActorBinding)
	// or NULL for legacy bindings without a UMovieSceneCustomBinding.
	// ls_asset_id deliberately FK-free; same reason as level_sequence_directors above.
	RawDB->Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS level_sequence_bindings ("
		"  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  ls_asset_id INTEGER NOT NULL,"
		"  binding_guid TEXT NOT NULL,"
		"  binding_index INTEGER NOT NULL DEFAULT 0,"
		"  name TEXT,"
		"  kind TEXT NOT NULL,"
		"  bound_class TEXT,"
		"  custom_binding_class TEXT,"
		"  custom_binding_pretty TEXT,"
		"  track_count INTEGER NOT NULL DEFAULT 0"
		")"
	));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ls_bind_ls ON level_sequence_bindings(ls_asset_id)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ls_bind_kind ON level_sequence_bindings(kind)"));
	RawDB->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_ls_bind_custom ON level_sequence_bindings(custom_binding_class)"));

	bTablesCreated = true;
}

bool FLevelSequenceIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	EnsureTablesExist(DB);

	FSQLiteDatabase* RawDB = DB.GetRawDatabase();
	if (!RawDB) return false;

	ULevelSequence* Seq = Cast<ULevelSequence>(LoadedAsset);
	if (!Seq) return false;

	const FString LsPathEarly = Seq->GetPathName();

	// Look up any prior director row for this LS — both its id and the asset_id
	// it was created under (which may differ from current AssetId after a force
	// reindex, since core's `assets` table is wiped and autoincrement restarts).
	int64 OldDirId = -1, OldAssetId = -1;
	SelectExistingDirectorIdAndAssetId(RawDB, LsPathEarly, OldDirId, OldAssetId);

	// Wipe event_bindings + bindings under BOTH the current asset id and the old one.
	// Without the OldAssetId pass, prior-reindex rows would orphan and never
	// be cleaned because their ls_asset_id no longer matches anything we know.
	ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_event_bindings WHERE ls_asset_id = ?"), AssetId);
	ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_bindings WHERE ls_asset_id = ?"), AssetId);
	if (OldAssetId > 0 && OldAssetId != AssetId)
	{
		ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_event_bindings WHERE ls_asset_id = ?"), OldAssetId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_bindings WHERE ls_asset_id = ?"), OldAssetId);
	}

#if WITH_EDITORONLY_DATA
	// Index all bindings (one row per Guid×BindingIndex) BEFORE the Director early-return,
	// so cinematics with no Director still get their binding inventory recorded.
	if (UMovieScene* MS = Seq->GetMovieScene())
	{
		const UMovieScene* CMSBindings = MS;
		const FMovieSceneBindingReferences* BindingRefs = Seq->GetBindingReferences();

		for (const FMovieSceneBinding& Binding : CMSBindings->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			const FString GuidStr = Guid.ToString(EGuidFormats::Digits);
			const int32 TrackCount = Binding.GetTracks().Num();
			// BindingReferences allows a priority-list of bindings per Guid; emit a row
			// per index so callers can see the full picture. When no references exist
			// for this Guid (pure legacy possessable/spawnable), emit one row at index 0.
			const int32 NumRefs = BindingRefs ? BindingRefs->GetReferences(Guid).Num() : 0;
			const int32 RowsForGuid = FMath::Max(1, NumRefs);

			for (int32 Idx = 0; Idx < RowsForGuid; ++Idx)
			{
				FString Name, Kind, BoundClass, CustomClass, CustomPretty;
				ClassifyBinding(Seq, MS, Guid, Idx, Name, Kind, BoundClass, CustomClass, CustomPretty);
				InsertBindingRow(RawDB, AssetId, GuidStr, Idx, Name, Kind, BoundClass, CustomClass, CustomPretty, TrackCount);
			}
		}
	}

	UBlueprint* DirBP = Seq->GetDirectorBlueprint();
	if (!DirBP)
	{
		// No Director — also wipe any leftover director/function/variable rows.
		if (OldDirId > 0)
		{
			ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_director_functions WHERE director_id = ?"), OldDirId);
			ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_director_variables WHERE director_id = ?"), OldDirId);
			ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_directors WHERE id = ?"), OldDirId);
		}
		return true;
	}

	const FString& LsPath = LsPathEarly;
	const FString DirName = DirBP->GetName();

	// Collect own-functions of this Director, classified by origin.
	// Mirrors MonolithBlueprint's convention: skip inherited base-class
	// methods and compiler-generated UE bytecode dispatchers.
	struct FFnRecord { FString Name; FString Kind; FString SignatureJson; };
	TArray<FFnRecord> Functions;

	for (UEdGraph* FuncGraph : DirBP->FunctionGraphs)
	{
		if (!FuncGraph) continue;
		FFnRecord Rec;
		Rec.Name = FuncGraph->GetFName().ToString();
		Rec.Kind = TEXT("user");
		Rec.SignatureJson = ExtractUserFunctionSignature(FuncGraph);
		Functions.Add(MoveTemp(Rec));
	}

	for (UEdGraph* UberGraph : DirBP->UbergraphPages)
	{
		if (!UberGraph) continue;
		for (UEdGraphNode* Node : UberGraph->Nodes)
		{
			UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
			if (!CustomEvent) continue;

			FFnRecord Rec;
			Rec.Name = CustomEvent->CustomFunctionName.ToString();
			Rec.Kind = TEXT("custom_event");
			Rec.SignatureJson = ExtractCustomEventSignature(CustomEvent);
			Functions.Add(MoveTemp(Rec));
		}
	}

	// UE-generated UFunctions on the compiled class — capture only the ones
	// declared on THIS class (skip inherited) AND not already present in our
	// graph-derived lists AND not the compiler's ExecuteUbergraph dispatchers.
	// Whatever survives is a Sequencer "Quick Bind" / "Create New Endpoint"
	// synthetic function (name pattern: SequenceEvent__ENTRYPOINT<DirBP>_N) —
	// it has no graph node but is the real target of
	// FMovieSceneEvent::Ptrs::Function at runtime. Indexing these is what
	// lets event_bindings.fires_function_id resolve.
	if (UClass* GenClass = DirBP->GeneratedClass)
	{
		TSet<FString> AlreadySeen;
		for (const FFnRecord& Rec : Functions) { AlreadySeen.Add(Rec.Name); }

		for (TFieldIterator<UFunction> It(GenClass); It; ++It)
		{
			UFunction* Fn = *It;
			if (!Fn) continue;

			// Skip inherited (base-class) functions — convention from blueprint_query.
			if (Fn->GetOwnerClass() != GenClass) continue;

			const FString FnName = Fn->GetName();

			// Skip compiler-generated ubergraph dispatchers (implementation detail).
			if (FnName == TEXT("ExecuteUbergraph") || FnName.StartsWith(TEXT("ExecuteUbergraph_")))
			{
				continue;
			}

			// Skip if already collected from a graph above.
			if (AlreadySeen.Contains(FnName)) continue;

			FFnRecord Rec;
			Rec.Name = FnName;
			Rec.Kind = TEXT("sequencer_endpoint");
			Rec.SignatureJson = TEXT("[]");
			Functions.Add(MoveTemp(Rec));
			AlreadySeen.Add(FnName);
		}
	}

	const int32 TotalFuncCount = Functions.Num();
	const int32 VarCount = DirBP->NewVariables.Num();

	// Clean prior director's children + director row (event_bindings already cleaned above).
	if (OldDirId > 0)
	{
		ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_director_functions WHERE director_id = ?"), OldDirId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_director_variables WHERE director_id = ?"), OldDirId);
		ExecWithInt64(RawDB, TEXT("DELETE FROM level_sequence_directors WHERE id = ?"), OldDirId);
	}

	// Insert fresh director row.
	int64 DirectorId = -1;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO level_sequence_directors "
			"(ls_asset_id, ls_path, director_bp_name, function_count, variable_count) "
			"VALUES (?, ?, ?, ?, ?)")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		Stmt.SetBindingValueByIndex(2, LsPath);
		Stmt.SetBindingValueByIndex(3, DirName);
		Stmt.SetBindingValueByIndex(4, TotalFuncCount);
		Stmt.SetBindingValueByIndex(5, VarCount);
		const bool bOk = Stmt.Execute();
		Stmt.Destroy();
		if (!bOk) return false;
		DirectorId = RawDB->GetLastInsertRowId();
	}

	// Insert functions; remember name -> row-id (the post-pass UPDATE below is
	// the actual source-of-truth for binding resolution, but we keep this map
	// so InsertEventBindingRow can opportunistically populate fires_function_id
	// inline when both function and binding sit in the same IndexAsset call).
	TMap<FName, int64> FuncNameToId;
	for (const FFnRecord& Fn : Functions)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO level_sequence_director_functions "
			"(director_id, name, kind, signature_json) "
			"VALUES (?, ?, ?, ?)")))
		{
			continue;
		}
		Stmt.SetBindingValueByIndex(1, DirectorId);
		Stmt.SetBindingValueByIndex(2, Fn.Name);
		Stmt.SetBindingValueByIndex(3, Fn.Kind);
		BindNullableString(Stmt, 4, Fn.SignatureJson);
		const bool bOk = Stmt.Execute();
		Stmt.Destroy();
		if (bOk)
		{
			FuncNameToId.Add(FName(*Fn.Name), RawDB->GetLastInsertRowId());
		}
	}

	// Insert variables.
	for (const FBPVariableDescription& Var : DirBP->NewVariables)
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*RawDB, TEXT(
			"INSERT INTO level_sequence_director_variables "
			"(director_id, name, type) "
			"VALUES (?, ?, ?)")))
		{
			continue;
		}
		Stmt.SetBindingValueByIndex(1, DirectorId);
		Stmt.SetBindingValueByIndex(2, Var.VarName.ToString());
		Stmt.SetBindingValueByIndex(3, VarTypeToString(Var.VarType));
		Stmt.Execute();
		Stmt.Destroy();
	}

	// Walk event tracks and record one row per FMovieSceneEvent.
	// fires_function_id is left NULL here for any rows we couldn't resolve
	// inline; a post-pass UPDATE below resolves them via SQL JOIN against
	// level_sequence_director_functions for this asset.
	if (UMovieScene* MS = Seq->GetMovieScene())
	{
		// Master tracks (not bound to a binding GUID).
		for (UMovieSceneTrack* Track : MS->GetTracks())
		{
			if (UMovieSceneEventTrack* EvTrack = Cast<UMovieSceneEventTrack>(Track))
			{
				ProcessEventTrack(EvTrack, RawDB, AssetId, FString(), FString(), TEXT("master"), FString(), FuncNameToId);
			}
		}

		// Object-bound tracks. Use the const overload of GetBindings (the non-const one
		// is UE_DEPRECATED(5.7)). FindPossessable/FindSpawnable still need non-const MS.
		const UMovieScene* CMS = MS;
		for (const FMovieSceneBinding& Binding : CMS->GetBindings())
		{
			const FGuid Guid = Binding.GetObjectGuid();
			FString BindingName, BindingKind, BoundClass;
			ResolveBinding(Seq, MS, Guid, BindingName, BindingKind, BoundClass);

			const FString GuidStr = Guid.ToString(EGuidFormats::Digits);
			for (UMovieSceneTrack* Track : Binding.GetTracks())
			{
				if (UMovieSceneEventTrack* EvTrack = Cast<UMovieSceneEventTrack>(Track))
				{
					ProcessEventTrack(EvTrack, RawDB, AssetId, GuidStr, BindingName, BindingKind, BoundClass, FuncNameToId);
				}
			}
		}

		// Post-pass: resolve fires_function_id by joining on (director's asset, function name).
		// Done in SQL because the in-process FuncNameToId map proved unreliable across
		// the indexer's threading context — a JOIN-based resolve is robust regardless.
		// Both ? bindings receive the same AssetId.
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*RawDB, TEXT(
			"UPDATE level_sequence_event_bindings AS b "
			"SET fires_function_id = ("
			"  SELECT f.id FROM level_sequence_director_functions f "
			"  JOIN level_sequence_directors d ON f.director_id = d.id "
			"  WHERE d.ls_asset_id = ? AND f.name = b.fires_function_name "
			"  LIMIT 1"
			") "
			"WHERE b.ls_asset_id = ? AND b.fires_function_id IS NULL AND b.fires_function_name IS NOT NULL")))
		{
			Stmt.SetBindingValueByIndex(1, AssetId);
			Stmt.SetBindingValueByIndex(2, AssetId);
			Stmt.Execute();
		}
		Stmt.Destroy();
	}
#endif

	return true;
}
