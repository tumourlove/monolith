#include "MonolithChooserActions.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_CHOOSER
#include "MonolithAssetUtils.h"
#include "MonolithChooserTreeCollector.h"

// Chooser runtime headers. Chooser.Build.cs adds its Internal/ dir to
// PublicIncludePaths, so the Internal table/result headers are reachable for any
// module taking the "Chooser" dependency.
#include "Chooser.h"                 // UChooserTable (Internal, on public include path)
#include "ChooserSignature.h"        // UChooserSignature (Public)
#include "ChooserPropertyAccess.h"   // FContextObjectTypeBase/Class/Struct + FChooserPropertyBinding (Public)
#include "ObjectChooser_Asset.h"     // FAssetChooser / FSoftAssetChooser (Internal, public path)
#include "OutputObjectColumn.h"       // FOutputObjectColumn / FChooserOutputObjectRowData (Internal, public path)
#include "BoolColumn.h"              // FBoolColumn / EBoolColumnCellValue (Internal, public path)
#include "EnumColumn.h"              // FEnumColumn / FChooserEnumRowData (Internal, public path)
#include "GameplayTagColumn.h"       // FGameplayTagColumn (RowValues: FGameplayTagContainer)
#include "FloatRangeColumn.h"        // FFloatRangeColumn / FChooserFloatRangeRowData
#include "GameplayTagContainer.h"

#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SoftObjectPath.h"

// DuplicateAsset (EditorScriptingUtilities — already a MonolithAnimation dep).
#include "EditorAssetLibrary.h"
#endif // WITH_CHOOSER

// ---------------------------------------------------------------------------
// Registration (always registers; per-handler gating below)
// ---------------------------------------------------------------------------

void FMonolithChooserActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// --- inspect_chooser ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("inspect_chooser"),
		TEXT("Read-only inspection of a UChooserTable: context-data parameters (class/struct requirements), result type + result class, row count, column count + types, a richer 'columns' array (per-column index, type, input-binding chain, is_input flag), referenced assets walked from result rows, and compile/validation status. Set include_cells=true to also emit per-row cell values for input columns (bool/enum/float-range/gameplay-tag). Set recursive=true to additionally emit the full nested tree of row/result targets (asset / soft_asset / evaluate_chooser / nested_chooser kinds), output-object column cells, fallback, parent_table/root_chooser, and recursed child tables."),
		FMonolithActionHandler::CreateStatic(&HandleInspectChooser),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Optional(TEXT("include_cells"), TEXT("bool"), TEXT("When true, emit per-row cell values for each input column under columns[].cells (bool MatchTrue/MatchFalse/any, enum value + comparison, float-range {min,max}, gameplay-tag list). Default false (back-compat: binding chains are always emitted, cells are not)."))
			.Optional(TEXT("recursive"), TEXT("bool"), TEXT("When true, emit a recursive 'tree' of every row/result target (normalized asset paths) including nested FEvaluateChooser/FNestedChooser children, FallbackResult, and FOutputObjectColumn cells, so a root->child remap is provable from readback. Default false (back-compat)."))
			.Build());

	// --- duplicate_chooser_tree ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("duplicate_chooser_tree"),
		TEXT("Duplicate one or more chooser tables into a destination folder (sources are never mutated). Optionally remap RootChooser/ParentTable/NestedChoosers and result asset references per remap_rules (map of old-path -> new-path)."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateChooserTree),
		FParamSchemaBuilder()
			.Required(TEXT("source_assets"), TEXT("array"), TEXT("Array of source UChooserTable asset paths to duplicate"))
			.Required(TEXT("destination_folder"), TEXT("string"), TEXT("Destination content folder, e.g. /Game/Tests/Monolith"))
			.Optional(TEXT("remap_rules"), TEXT("object"), TEXT("Optional map of old-asset-path -> new-asset-path applied to RootChooser/ParentTable/NestedChoosers and result FInstancedStruct asset refs in each duplicate"))
			.Build());

	// --- set_context_object_class ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("set_context_object_class"),
		TEXT("Rewrite the Class on a ContextData parameter entry (FContextObjectTypeClass) of a chooser table, e.g. to ABP_Humanoid_C. Marks the package dirty and recompiles (Compile(true))."),
		FMonolithActionHandler::CreateStatic(&HandleSetContextObjectClass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Required(TEXT("context_name_or_index"), TEXT("string"), TEXT("Index of the ContextData entry to retarget (0-based). A non-numeric value selects the first class-typed context entry."))
			.Required(TEXT("class_path"), TEXT("string"), TEXT("New context object class, e.g. /Game/.../ABP_Humanoid.ABP_Humanoid_C or a loaded class name"))
			.Build());

	// --- set_result_asset_reference ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("set_result_asset_reference"),
		TEXT("Rewrite the Asset reference on a result row (FAssetChooser / FSoftAssetChooser) of a chooser table, e.g. a PoseSearch database. Marks the package dirty and recompiles (Compile(true))."),
		FMonolithActionHandler::CreateStatic(&HandleSetResultAssetReference),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Required(TEXT("row_or_column"), TEXT("number"), TEXT("0-based result row index whose asset reference to rewrite"))
			.Required(TEXT("asset_path_value"), TEXT("string"), TEXT("New asset path to assign to the result row's Asset reference"))
			.Build());

	// --- set_evaluate_chooser_result_reference ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("set_evaluate_chooser_result_reference"),
		TEXT("Rewrite the child UChooserTable an EvaluateChooser result row points at (FEvaluateChooser). Root/nested chooser rows are EvaluateChooser and are unsettable via set_result_asset_reference; this action handles them. Marks the package dirty and recompiles (Compile(true))."),
		FMonolithActionHandler::CreateStatic(&HandleSetEvaluateChooserResultReference),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset to edit"))
			.Required(TEXT("row"), TEXT("number"), TEXT("0-based result row index of the EvaluateChooser row to rewrite"))
			.RequiredAssetPath(TEXT("child_chooser_path"), TEXT("UChooserTable to point the EvaluateChooser row at"))
			.Build());

	// --- validate_chooser ---
	Registry.RegisterAction(TEXT("chooser"), TEXT("validate_chooser"),
		TEXT("Compile a chooser table (Compile(true)) and validate it: optional expected context class + expected result type, plus null/stale result-row asset references. Read-only apart from the compile pass."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChooser),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UChooserTable asset path"))
			.Optional(TEXT("expected_context_class"), TEXT("string"), TEXT("Optional: class expected on at least one ContextData entry"))
			.Optional(TEXT("expected_result_type"), TEXT("string"), TEXT("Optional: expected result type — ObjectResult, ClassResult, or NoPrimaryResult"))
			.Build());
}

// ===========================================================================
// WITH_CHOOSER == 0 : clean off-gate stubs
// ===========================================================================
#if !WITH_CHOOSER

namespace
{
	FMonolithActionResult ChooserUnavailable()
	{
		return FMonolithActionResult::Error(TEXT("Chooser plugin not available"));
	}
}

FMonolithActionResult FMonolithChooserActions::HandleInspectChooser(const TSharedPtr<FJsonObject>&)        { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleDuplicateChooserTree(const TSharedPtr<FJsonObject>&)  { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleSetContextObjectClass(const TSharedPtr<FJsonObject>&) { return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleSetResultAssetReference(const TSharedPtr<FJsonObject>&){ return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleSetEvaluateChooserResultReference(const TSharedPtr<FJsonObject>&){ return ChooserUnavailable(); }
FMonolithActionResult FMonolithChooserActions::HandleValidateChooser(const TSharedPtr<FJsonObject>&)       { return ChooserUnavailable(); }

#else // WITH_CHOOSER

// ===========================================================================
// Helpers
// ===========================================================================

namespace
{
	/** Human-readable name for the chooser result type enum. */
	FString ResultTypeToString(EObjectChooserResultType Type)
	{
		switch (Type)
		{
		case EObjectChooserResultType::ObjectResult:    return TEXT("ObjectResult");
		case EObjectChooserResultType::ClassResult:     return TEXT("ClassResult");
		case EObjectChooserResultType::NoPrimaryResult: return TEXT("NoPrimaryResult");
		default:                                        return TEXT("Unknown");
		}
	}

	/** Resolve a result type string (case-insensitive) to the enum. Returns false if unrecognized. */
	bool ParseResultType(const FString& Str, EObjectChooserResultType& Out)
	{
		if (Str.Equals(TEXT("ObjectResult"), ESearchCase::IgnoreCase))    { Out = EObjectChooserResultType::ObjectResult;    return true; }
		if (Str.Equals(TEXT("ClassResult"), ESearchCase::IgnoreCase))     { Out = EObjectChooserResultType::ClassResult;     return true; }
		if (Str.Equals(TEXT("NoPrimaryResult"), ESearchCase::IgnoreCase)) { Out = EObjectChooserResultType::NoPrimaryResult; return true; }
		return false;
	}

	/**
	 * Extract the referenced asset (if any) from a result-row FInstancedStruct.
	 * Handles the hard FAssetChooser and soft FSoftAssetChooser struct types.
	 * Returns the asset path, or empty string if the row is not an asset result.
	 * bOutIsNull is set true when the row IS an asset result but the reference is unset.
	 */
	FString GetRowAssetPath(const FInstancedStruct& Row, bool& bOutIsNull, FString& OutStructType)
	{
		bOutIsNull = false;
		OutStructType.Reset();

		const UScriptStruct* SS = Row.GetScriptStruct();
		if (!SS)
		{
			return FString();
		}
		OutStructType = SS->GetName();

		if (const FAssetChooser* Hard = Row.GetPtr<FAssetChooser>())
		{
			if (Hard->Asset)
			{
				return Hard->Asset->GetPathName();
			}
			bOutIsNull = true;
			return FString();
		}
		if (const FSoftAssetChooser* Soft = Row.GetPtr<FSoftAssetChooser>())
		{
			const FSoftObjectPath SoftPath = Soft->Asset.ToSoftObjectPath();
			if (SoftPath.IsValid())
			{
				return SoftPath.ToString();
			}
			bOutIsNull = true;
			return FString();
		}
		return FString();
	}

	/** Combine a destination folder + a source asset's short name into a full dest asset path. */
	FString MakeDestAssetPath(const FString& DestFolder, const FString& SourceAssetPath)
	{
		FString Folder = DestFolder;
		while (Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}
		const FString ShortName = FMonolithAssetUtils::GetAssetName(SourceAssetPath);
		return Folder + TEXT("/") + ShortName;
	}

	/** True when the column is one of the four bindable input (filter) column types. */
	bool IsInputColumn(const FInstancedStruct& Col)
	{
		return Col.GetPtr<FBoolColumn>() || Col.GetPtr<FEnumColumn>()
			|| Col.GetPtr<FGameplayTagColumn>() || Col.GetPtr<FFloatRangeColumn>();
	}

	/**
	 * Read a column's input-binding chain into a JSON object { chain:[names], display:"A.B" }.
	 * Mirrors ApplyInputBinding (MonolithChooserAuthoringActions.cpp) const-side: reach the
	 * column's InputValue FInstancedStruct, find the 'Binding' FStructProperty on the
	 * context-property struct (derives FChooserPropertyBinding), and read PropertyBindingChain.
	 * Returns null when the column has no resolvable input binding (output/unknown columns,
	 * or an input column whose InputValue/Binding struct could not be reached).
	 */
	TSharedPtr<FJsonObject> ReadInputBinding(const FInstancedStruct& Col)
	{
		const FInstancedStruct* InputValue = nullptr;
		if (const FBoolColumn* BoolC = Col.GetPtr<FBoolColumn>())              { InputValue = &BoolC->InputValue; }
		else if (const FEnumColumn* EnumC = Col.GetPtr<FEnumColumn>())         { InputValue = &EnumC->InputValue; }
		else if (const FGameplayTagColumn* TagC = Col.GetPtr<FGameplayTagColumn>()) { InputValue = &TagC->InputValue; }
		else if (const FFloatRangeColumn* RangeC = Col.GetPtr<FFloatRangeColumn>()) { InputValue = &RangeC->InputValue; }
		if (!InputValue || !InputValue->IsValid())
		{
			return nullptr;
		}

		const UScriptStruct* SS = InputValue->GetScriptStruct();
		if (!SS)
		{
			return nullptr;
		}

		for (TFieldIterator<FStructProperty> It(SS); It; ++It)
		{
			if (It->GetName() != TEXT("Binding"))
			{
				continue;
			}
			if (!It->Struct || !It->Struct->IsChildOf(FChooserPropertyBinding::StaticStruct()))
			{
				break;
			}
			const void* BindingPtr = It->ContainerPtrToValuePtr<void>(InputValue->GetMemory());
			if (!BindingPtr)
			{
				break;
			}
			const FChooserPropertyBinding* Binding = static_cast<const FChooserPropertyBinding*>(BindingPtr);

			TArray<TSharedPtr<FJsonValue>> ChainArr;
			TArray<FString> ChainStrs;
			for (const FName& Link : Binding->PropertyBindingChain)
			{
				ChainArr.Add(MakeShared<FJsonValueString>(Link.ToString()));
				ChainStrs.Add(Link.ToString());
			}

			TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
			BindingObj->SetArrayField(TEXT("chain"), ChainArr);
			BindingObj->SetStringField(TEXT("display"), FString::Join(ChainStrs, TEXT(".")));
			return BindingObj;
		}
		return nullptr;
	}

	/** Bool cell-value enum -> JSON-friendly string. */
	FString BoolCellToString(EBoolColumnCellValue Value)
	{
		switch (Value)
		{
		case EBoolColumnCellValue::MatchFalse: return TEXT("false");
		case EBoolColumnCellValue::MatchTrue:  return TEXT("true");
		case EBoolColumnCellValue::MatchAny:   return TEXT("any");
		default:                               return TEXT("unknown");
		}
	}

	/**
	 * Append per-row cell values for an input column into OutCells. Handles the four
	 * input column types; no-op for output/unknown columns. Float-range honors the
	 * infinite-bound flags (bNoMin/bNoMax) by emitting nulls for unbounded ends.
	 */
	void ReadColumnCells(const FInstancedStruct& Col, TArray<TSharedPtr<FJsonValue>>& OutCells)
	{
		if (const FBoolColumn* BoolC = Col.GetPtr<FBoolColumn>())
		{
			for (int32 r = 0; r < BoolC->RowValuesWithAny.Num(); ++r)
			{
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("row"), r);
				Cell->SetStringField(TEXT("value"), BoolCellToString(BoolC->RowValuesWithAny[r]));
				OutCells.Add(MakeShared<FJsonValueObject>(Cell));
			}
			return;
		}
		if (const FFloatRangeColumn* RangeC = Col.GetPtr<FFloatRangeColumn>())
		{
			for (int32 r = 0; r < RangeC->RowValues.Num(); ++r)
			{
				const FChooserFloatRangeRowData& Row = RangeC->RowValues[r];
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("row"), r);
				if (Row.bNoMin) { Cell->SetField(TEXT("min"), MakeShared<FJsonValueNull>()); }
				else            { Cell->SetNumberField(TEXT("min"), Row.Min); }
				if (Row.bNoMax) { Cell->SetField(TEXT("max"), MakeShared<FJsonValueNull>()); }
				else            { Cell->SetNumberField(TEXT("max"), Row.Max); }
				OutCells.Add(MakeShared<FJsonValueObject>(Cell));
			}
			return;
		}
		if (const FGameplayTagColumn* TagC = Col.GetPtr<FGameplayTagColumn>())
		{
			for (int32 r = 0; r < TagC->RowValues.Num(); ++r)
			{
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("row"), r);
				TArray<TSharedPtr<FJsonValue>> TagArr;
				for (const FGameplayTag& Tag : TagC->RowValues[r])
				{
					TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
				}
				Cell->SetArrayField(TEXT("tags"), TagArr);
				OutCells.Add(MakeShared<FJsonValueObject>(Cell));
			}
			return;
		}
		if (const FEnumColumn* EnumC = Col.GetPtr<FEnumColumn>())
		{
			for (int32 r = 0; r < EnumC->RowValues.Num(); ++r)
			{
				const FChooserEnumRowData& Row = EnumC->RowValues[r];
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("row"), r);
				Cell->SetNumberField(TEXT("value"), Row.Value);
				Cell->SetNumberField(TEXT("comparison"), static_cast<int32>(Row.Comparison));
				OutCells.Add(MakeShared<FJsonValueObject>(Cell));
			}
			return;
		}
		// Output / unknown column types: no cells emitted.
	}
}

// ===========================================================================
// inspect_chooser
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleInspectChooser(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());

	// Result type + class (from parent UChooserSignature)
	Root->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Root->SetNumberField(TEXT("result_type_value"), static_cast<int32>(Table->ResultType));
	Root->SetStringField(TEXT("result_class"), Table->OutputObjectType ? Table->OutputObjectType->GetPathName() : TEXT(""));

	// Context data parameters. GetContextData() returns a view from the root chooser;
	// do not store the view past this scope.
	{
		TArray<TSharedPtr<FJsonValue>> ContextArr;
		const TConstArrayView<FInstancedStruct> ContextView = Table->GetContextData();
		for (int32 i = 0; i < ContextView.Num(); ++i)
		{
			const FInstancedStruct& Entry = ContextView[i];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("index"), i);
			const UScriptStruct* SS = Entry.GetScriptStruct();
			Obj->SetStringField(TEXT("struct_type"), SS ? SS->GetName() : TEXT(""));
			if (const FContextObjectTypeClass* AsClass = Entry.GetPtr<FContextObjectTypeClass>())
			{
				Obj->SetStringField(TEXT("kind"), TEXT("class"));
				Obj->SetStringField(TEXT("class"), AsClass->Class ? AsClass->Class->GetPathName() : TEXT(""));
			}
			else if (const FContextObjectTypeStruct* AsStruct = Entry.GetPtr<FContextObjectTypeStruct>())
			{
				Obj->SetStringField(TEXT("kind"), TEXT("struct"));
				Obj->SetStringField(TEXT("struct"), AsStruct->Struct ? AsStruct->Struct->GetPathName() : TEXT(""));
			}
			else
			{
				Obj->SetStringField(TEXT("kind"), TEXT("other"));
			}
			ContextArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Root->SetArrayField(TEXT("context_data"), ContextArr);
	}

	// Rows + columns + referenced assets are WITH_EDITORONLY_DATA on the table.
#if WITH_EDITORONLY_DATA
	Root->SetNumberField(TEXT("row_count"), Table->ResultsStructs.Num());
	Root->SetNumberField(TEXT("column_count"), Table->ColumnsStructs.Num());

	{
		TArray<TSharedPtr<FJsonValue>> ColTypes;
		for (const FInstancedStruct& Col : Table->ColumnsStructs)
		{
			const UScriptStruct* SS = Col.GetScriptStruct();
			ColTypes.Add(MakeShared<FJsonValueString>(SS ? SS->GetName() : TEXT("<null>")));
		}
		Root->SetArrayField(TEXT("column_types"), ColTypes);
	}

	// Richer per-column view: index, type, input-binding chain, is_input flag, and
	// (when include_cells) per-row cell values. Unknown/output column types degrade to
	// { type, is_input:false } with no binding — never an error.
	{
		bool bIncludeCells = false;
		Params->TryGetBoolField(TEXT("include_cells"), bIncludeCells);

		TArray<TSharedPtr<FJsonValue>> ColumnsArr;
		for (int32 c = 0; c < Table->ColumnsStructs.Num(); ++c)
		{
			const FInstancedStruct& Col = Table->ColumnsStructs[c];
			const UScriptStruct* SS = Col.GetScriptStruct();

			TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
			ColObj->SetNumberField(TEXT("index"), c);
			ColObj->SetStringField(TEXT("type"), SS ? SS->GetName() : TEXT("<null>"));

			const bool bIsInput = IsInputColumn(Col);
			ColObj->SetBoolField(TEXT("is_input"), bIsInput);
			if (TSharedPtr<FJsonObject> Binding = ReadInputBinding(Col))
			{
				ColObj->SetObjectField(TEXT("input_binding"), Binding);
			}

			if (bIncludeCells && bIsInput)
			{
				TArray<TSharedPtr<FJsonValue>> Cells;
				ReadColumnCells(Col, Cells);
				ColObj->SetArrayField(TEXT("cells"), Cells);
			}

			ColumnsArr.Add(MakeShared<FJsonValueObject>(ColObj));
		}
		Root->SetArrayField(TEXT("columns"), ColumnsArr);
	}

	{
		TArray<TSharedPtr<FJsonValue>> RefAssets;
		for (int32 r = 0; r < Table->ResultsStructs.Num(); ++r)
		{
			bool bIsNull = false;
			FString StructType;
			const FString AssetRef = GetRowAssetPath(Table->ResultsStructs[r], bIsNull, StructType);
			if (!AssetRef.IsEmpty() || bIsNull)
			{
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetNumberField(TEXT("row"), r);
				Obj->SetStringField(TEXT("struct_type"), StructType);
				Obj->SetStringField(TEXT("asset"), AssetRef);
				Obj->SetBoolField(TEXT("is_null"), bIsNull);
				RefAssets.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		Root->SetArrayField(TEXT("referenced_assets"), RefAssets);
	}
#else
	Root->SetNumberField(TEXT("row_count"), Table->CookedResults.Num());
	Root->SetBoolField(TEXT("editor_only_data_available"), false);
#endif

	// Optional recursive tree: full nested walk via the shared read-only collector. The
	// collector classifies each row/result location by kind (asset / soft_asset /
	// evaluate_chooser / nested_chooser), resolves its target path, and recurses into embedded
	// child tables (NestedObjects) — so a root->child chooser remap is provable from readback.
	bool bRecursive = false;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);
	if (bRecursive)
	{
		// The visited-set cycle guard is a REQUIRED parameter of the collector's entry point.
		TSet<UChooserTable*> VisitedTables;
		Root->SetObjectField(TEXT("tree"), MonolithChooserTree::CollectTree(Table, VisitedTables));
	}

	// Compile to surface validation status (read-only side effect: regenerates cooked data).
	Table->Compile(/*bForce=*/false);
	Root->SetBoolField(TEXT("compiled"), true);

	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// duplicate_chooser_tree
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleDuplicateChooserTree(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* SourcesPtr = nullptr;
	if (!Params->TryGetArrayField(TEXT("source_assets"), SourcesPtr) || !SourcesPtr)
	{
		return FMonolithActionResult::Error(TEXT("Missing required array parameter: source_assets"));
	}
	const FString DestFolder = Params->GetStringField(TEXT("destination_folder"));
	if (DestFolder.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: destination_folder"));
	}

	// Normalize an asset reference to a comparable package path: strip any
	// "object-name" suffix ("/Game/X/CHT.CHT" -> "/Game/X/CHT") and any sub-object
	// suffix, and drop a trailing class-name part. UObject::GetPathName() yields the
	// "package.object" form, whereas remap_rules keys are typically bare package
	// paths ("/Game/X/CHT"); normalizing BOTH sides makes them compare equal.
	auto NormalizePackagePath = [](const FString& InPath) -> FString
	{
		FString Path = InPath;
		// Drop a sub-object path first ("/Game/X/CHT.CHT:Sub" -> "/Game/X/CHT.CHT").
		int32 SubIdx = INDEX_NONE;
		if (Path.FindChar(TEXT(':'), SubIdx))
		{
			Path.LeftInline(SubIdx, EAllowShrinking::No);
		}
		// Drop the object-name suffix ("/Game/X/CHT.CHT" -> "/Game/X/CHT").
		int32 DotIdx = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIdx))
		{
			Path.LeftInline(DotIdx, EAllowShrinking::No);
		}
		return Path;
	};

	// Build the remap table keyed on NORMALIZED package paths so GetPathName()
	// object-path lookups ("/Game/X/CHT.CHT") match bare package-path rules
	// ("/Game/X/CHT"). The value (new path) is preserved verbatim.
	TMap<FString, FString> Remap;
	const TSharedPtr<FJsonObject>* RemapObj = nullptr;
	if (Params->TryGetObjectField(TEXT("remap_rules"), RemapObj) && RemapObj && RemapObj->IsValid())
	{
		for (const auto& Pair : (*RemapObj)->Values)
		{
			FString Val;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
			{
				Remap.Add(NormalizePackagePath(*Pair.Key), Val);
			}
		}
	}

	auto ApplyRemap = [&Remap, &NormalizePackagePath](const FString& InPath) -> FString
	{
		// Try an exact match first (preserves prior behavior for already-matching keys),
		// then fall back to a normalized package-path match.
		if (const FString* Found = Remap.Find(InPath))
		{
			return *Found;
		}
		if (const FString* FoundNorm = Remap.Find(NormalizePackagePath(InPath)))
		{
			return *FoundNorm;
		}
		return InPath;
	};

	// One record per successfully-duplicated source. The remap walk in PASS 2 reads
	// these AFTER every duplicate exists on disk, so the remap-target lookup always
	// resolves regardless of the order sources are listed in source_assets.
	struct FDupRecord
	{
		TSharedPtr<FJsonObject> Entry;
		UObject* Dup = nullptr;
		UChooserTable* DupTable = nullptr;
	};
	TArray<FDupRecord> DupRecords;

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Duplicated = 0;

	// -----------------------------------------------------------------------
	// PASS 1: duplicate ALL sources first and SaveAsset each, so every
	// duplicate (and its new remap-target path) exists on disk before any
	// reference walk runs. No remap happens in this pass.
	// -----------------------------------------------------------------------
	for (const TSharedPtr<FJsonValue>& Val : *SourcesPtr)
	{
		FString SourcePath;
		if (!Val.IsValid() || !Val->TryGetString(SourcePath) || SourcePath.IsEmpty())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("source"), SourcePath);

		const FString DestPath = MakeDestAssetPath(DestFolder, SourcePath);
		UObject* Dup = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
		if (!Dup)
		{
			Entry->SetBoolField(TEXT("ok"), false);
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("DuplicateAsset failed -> %s"), *DestPath));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		Entry->SetStringField(TEXT("destination"), Dup->GetPathName());
		++Duplicated;

		// Persist the bare duplicate now so its new path is resolvable from disk
		// when PASS 2 walks any OTHER duplicate that references it.
		const bool bSaved = UEditorAssetLibrary::SaveAsset(Dup->GetPathName(), /*bOnlyIfIsDirty=*/false);
		Entry->SetBoolField(TEXT("saved"), bSaved);
		Entry->SetBoolField(TEXT("ok"), true);

		FDupRecord Rec;
		Rec.Entry = Entry;
		Rec.Dup = Dup;
		Rec.DupTable = Cast<UChooserTable>(Dup);
		DupRecords.Add(Rec);

		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// -----------------------------------------------------------------------
	// PASS 2: now that ALL duplicates exist on disk, remap references on each.
	// The remap lambdas write into the CURRENT record's RefsRemapped / RowReport
	// via these cursors, reset per iteration below.
	// -----------------------------------------------------------------------
	if (Remap.Num() > 0)
	{
		int32 CurRefsRemapped = 0;
		TArray<TSharedPtr<FJsonValue>> CurRowReport;

		auto AddRowReport = [&CurRowReport](int32 RowIndex, const FString& StructType,
			const FString& OldPath, const FString& NewPath, bool bRemapped, const FString& Note)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("row_index"), RowIndex);
			Obj->SetStringField(TEXT("struct_type"), StructType);
			Obj->SetStringField(TEXT("old_path"), OldPath);
			Obj->SetStringField(TEXT("new_path"), NewPath);
			Obj->SetBoolField(TEXT("remapped"), bRemapped);
			if (!Note.IsEmpty())
			{
				Obj->SetStringField(TEXT("note"), Note);
			}
			CurRowReport.Add(MakeShared<FJsonValueObject>(Obj));
		};

#if WITH_EDITORONLY_DATA
		int32& RefsRemapped = CurRefsRemapped;
		// Remap a single result FInstancedStruct (a result row, the table's
			// FallbackResult, or an FOutputObjectColumn cell value). Handles all four
			// chooser result struct types and ALWAYS emits a diagnostic report entry,
			// including for structs that match none of the known branches. This makes an
			// empty row_remap_report impossible and records the actual struct type name so
			// a future run shows exactly what each location holds.
			//
			// `Where` labels the container (e.g. "row", "fallback", "column[2].row[3]");
			// `RowIdx` is the row index for ResultsStructs, INDEX_NONE for non-row sites.
			auto RemapResultStruct =
				[&AddRowReport, &ApplyRemap, &RefsRemapped]
				(FInstancedStruct& S, int32 RowIdx, const FString& Where)
			{
				if (FAssetChooser* Hard = S.GetMutablePtr<FAssetChooser>())
				{
					if (Hard->Asset)
					{
						const FString OldRef = Hard->Asset->GetPathName();
						const FString NewRef = ApplyRemap(OldRef);
						if (NewRef != OldRef)
						{
							if (UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(NewRef))
							{
								Hard->Asset = Loaded;
								++RefsRemapped;
								AddRowReport(RowIdx, FString::Printf(TEXT("FAssetChooser@%s"), *Where), OldRef, NewRef, true, FString());
							}
							else
							{
								AddRowReport(RowIdx, FString::Printf(TEXT("FAssetChooser@%s"), *Where), OldRef, NewRef, false,
									TEXT("remap target not found; kept original reference"));
							}
						}
						else
						{
							AddRowReport(RowIdx, FString::Printf(TEXT("FAssetChooser@%s"), *Where), OldRef, OldRef, false,
								TEXT("no remap rule matched this reference"));
						}
					}
					else
					{
						AddRowReport(RowIdx, FString::Printf(TEXT("FAssetChooser@%s"), *Where), FString(), FString(), false,
							TEXT("FAssetChooser with null Asset"));
					}
				}
				else if (FSoftAssetChooser* Soft = S.GetMutablePtr<FSoftAssetChooser>())
				{
					const FString OldRef = Soft->Asset.ToSoftObjectPath().ToString();
					const FString NewRef = ApplyRemap(OldRef);
					if (NewRef != OldRef)
					{
						// Soft references do not require the target to be loaded.
						Soft->Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(NewRef));
						++RefsRemapped;
						AddRowReport(RowIdx, FString::Printf(TEXT("FSoftAssetChooser@%s"), *Where), OldRef, NewRef, true, FString());
					}
					else
					{
						AddRowReport(RowIdx, FString::Printf(TEXT("FSoftAssetChooser@%s"), *Where), OldRef, OldRef, false,
							TEXT("no remap rule matched this reference"));
					}
				}
				else if (FEvaluateChooser* Eval = S.GetMutablePtr<FEvaluateChooser>())
				{
					// FEvaluateChooser references a SEPARATE chooser asset (root/nested
					// chooser rows). Without remapping these, the duplicate still points at
					// the ORIGINAL child chooser tables.
					if (Eval->Chooser)
					{
						const FString OldRef = Eval->Chooser->GetPathName();
						const FString NewRef = ApplyRemap(OldRef);
						if (NewRef != OldRef)
						{
							if (UChooserTable* NewChild = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewRef))
							{
								Eval->Chooser = NewChild;
								++RefsRemapped;
								AddRowReport(RowIdx, FString::Printf(TEXT("FEvaluateChooser@%s"), *Where), OldRef, NewRef, true, FString());
							}
							else
							{
								AddRowReport(RowIdx, FString::Printf(TEXT("FEvaluateChooser@%s"), *Where), OldRef, NewRef, false,
									TEXT("child chooser not found at remap target; kept original reference"));
							}
						}
						else
						{
							AddRowReport(RowIdx, FString::Printf(TEXT("FEvaluateChooser@%s"), *Where), OldRef, OldRef, false,
								TEXT("no remap rule matched this child chooser path"));
						}
					}
					else
					{
						AddRowReport(RowIdx, FString::Printf(TEXT("FEvaluateChooser@%s"), *Where), FString(), FString(), false,
							TEXT("FEvaluateChooser with null Chooser"));
					}
				}
				else if (FNestedChooser* Nested = S.GetMutablePtr<FNestedChooser>())
				{
					// FNestedChooser references a chooser table EMBEDDED in this asset.
					// Mirror the FEvaluateChooser branch: remap its child table reference.
					if (Nested->Chooser)
					{
						const FString OldRef = Nested->Chooser->GetPathName();
						const FString NewRef = ApplyRemap(OldRef);
						if (NewRef != OldRef)
						{
							if (UChooserTable* NewChild = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewRef))
							{
								Nested->Chooser = NewChild;
								++RefsRemapped;
								AddRowReport(RowIdx, FString::Printf(TEXT("FNestedChooser@%s"), *Where), OldRef, NewRef, true, FString());
							}
							else
							{
								AddRowReport(RowIdx, FString::Printf(TEXT("FNestedChooser@%s"), *Where), OldRef, NewRef, false,
									TEXT("nested chooser not found at remap target; kept original reference"));
							}
						}
						else
						{
							AddRowReport(RowIdx, FString::Printf(TEXT("FNestedChooser@%s"), *Where), OldRef, OldRef, false,
								TEXT("no remap rule matched this nested chooser path"));
						}
					}
					else
					{
						AddRowReport(RowIdx, FString::Printf(TEXT("FNestedChooser@%s"), *Where), FString(), FString(), false,
							TEXT("FNestedChooser with null Chooser"));
					}
				}
				else
				{
					// DIAGNOSTIC: a result struct that matches none of the known branches.
					// Record the actual struct type so a future run reveals what it is.
					const UScriptStruct* SS = S.GetScriptStruct();
					AddRowReport(RowIdx, FString::Printf(TEXT("%s@%s"),
						SS ? *SS->GetName() : TEXT("<null-struct>"), *Where),
						FString(), FString(), false,
						TEXT("unhandled result struct type; no remap attempted"));
				}
			};

			// Walk one table's three reference locations (ResultsStructs, FallbackResult,
			// FOutputObjectColumn cells) — mirrors the engine's ReplaceReferencesInTable
			// (SNestedChooserTree.cpp ~46-100). `Prefix` namespaces the diagnostic labels
			// so root-table sites read "row"/"fallback"/"column[..]" while embedded-child
			// sites read "nested[n].row"/"nested[n].fallback"/... .
			auto WalkTableReferences =
				[&RemapResultStruct](UChooserTable* Tbl, const FString& Prefix)
			{
				if (!Tbl)
				{
					return;
				}

				// 1) Result rows. Every row reports.
				for (int32 RowIdx = 0; RowIdx < Tbl->ResultsStructs.Num(); ++RowIdx)
				{
					RemapResultStruct(Tbl->ResultsStructs[RowIdx], RowIdx, Prefix + TEXT("row"));
				}

				// 2) The table's FallbackResult (a single FInstancedStruct).
				if (Tbl->FallbackResult.IsValid())
				{
					RemapResultStruct(Tbl->FallbackResult, INDEX_NONE, Prefix + TEXT("fallback"));
				}

				// 3) FOutputObjectColumn cell values. Each column is an FInstancedStruct in
				//    ColumnsStructs; for output-object columns, remap per-row RowValues[].Value,
				//    plus FallbackValue.Value and (editor-only) DefaultRowValue.Value.
				for (int32 ColIdx = 0; ColIdx < Tbl->ColumnsStructs.Num(); ++ColIdx)
				{
					FInstancedStruct& ColumnData = Tbl->ColumnsStructs[ColIdx];
					if (FOutputObjectColumn* OutCol = ColumnData.GetMutablePtr<FOutputObjectColumn>())
					{
						for (int32 CellIdx = 0; CellIdx < OutCol->RowValues.Num(); ++CellIdx)
						{
							if (OutCol->RowValues[CellIdx].Value.IsValid())
							{
								RemapResultStruct(OutCol->RowValues[CellIdx].Value, CellIdx,
									Prefix + FString::Printf(TEXT("column[%d].row[%d]"), ColIdx, CellIdx));
							}
						}
						if (OutCol->FallbackValue.Value.IsValid())
						{
							RemapResultStruct(OutCol->FallbackValue.Value, INDEX_NONE,
								Prefix + FString::Printf(TEXT("column[%d].fallback"), ColIdx));
						}
						if (OutCol->DefaultRowValue.Value.IsValid())
						{
							RemapResultStruct(OutCol->DefaultRowValue.Value, INDEX_NONE,
								Prefix + FString::Printf(TEXT("column[%d].default"), ColIdx));
						}
					}
				}
			};
#endif // WITH_EDITORONLY_DATA (RemapResultStruct / WalkTableReferences definitions)

		// Run the remap over EVERY duplicate, now that all of them (and their new
		// remap-target paths) exist on disk from PASS 1. Order-independent.
		for (FDupRecord& Rec : DupRecords)
		{
			UChooserTable* DupTable = Rec.DupTable;
			if (!DupTable)
			{
				continue;
			}

			// Reset the cursors the lambdas write into for this duplicate.
			CurRefsRemapped = 0;
			CurRowReport.Reset();

#if WITH_EDITORONLY_DATA
			// Walk the root duplicate's own references...
			WalkTableReferences(DupTable, FString());

			// ...then recurse into embedded child choosers. The engine's ReplaceReferences
			// (SNestedChooserTree.cpp ~104-121) iterates RootTable->NestedObjects, casts each
			// to UChooserTable, and runs ReplaceReferencesInTable on every embedded child.
			// For the pose-search root chooser, the child choosers are embedded NestedObjects
			// and THEIR FNestedChooser/FEvaluateChooser rows hold the unremapped references.
			for (int32 NestedIdx = 0; NestedIdx < DupTable->NestedObjects.Num(); ++NestedIdx)
			{
				if (UChooserTable* NestedChild = Cast<UChooserTable>(DupTable->NestedObjects[NestedIdx]))
				{
					WalkTableReferences(NestedChild, FString::Printf(TEXT("nested[%d]."), NestedIdx));
				}
			}

			// Remap structural parent/nested links by reloading the remapped target.
			if (DupTable->ParentTable)
			{
				const FString NewParent = ApplyRemap(DupTable->ParentTable->GetPathName());
				if (UChooserTable* P = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewParent))
				{
					if (P != DupTable->ParentTable) { DupTable->ParentTable = P; ++RefsRemapped; }
				}
			}
#endif // WITH_EDITORONLY_DATA

			if (DupTable->RootChooser)
			{
				const FString NewRoot = ApplyRemap(DupTable->RootChooser->GetPathName());
				if (UChooserTable* R = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(NewRoot))
				{
					if (R != DupTable->RootChooser) { DupTable->RootChooser = R; ++CurRefsRemapped; }
				}
			}

			DupTable->MarkPackageDirty();
			DupTable->Compile(/*bForce=*/true);

			// Re-save now that references are remapped, so a re-test reading from
			// disk sees the remapped refs (PASS 1 saved only the bare duplicate).
			const bool bResaved = UEditorAssetLibrary::SaveAsset(Rec.Dup->GetPathName(), /*bOnlyIfIsDirty=*/false);

			Rec.Entry->SetNumberField(TEXT("refs_remapped"), CurRefsRemapped);
			Rec.Entry->SetArrayField(TEXT("row_remap_report"), CurRowReport);
			Rec.Entry->SetBoolField(TEXT("saved"), bResaved);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("duplicated"), Duplicated);
	Root->SetArrayField(TEXT("results"), Results);
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// set_context_object_class
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleSetContextObjectClass(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString IndexStr  = Params->GetStringField(TEXT("context_name_or_index"));
	const FString ClassPath = Params->GetStringField(TEXT("class_path"));

	if (ClassPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: class_path"));
	}

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	UClass* NewClass = LoadClass<UObject>(nullptr, *ClassPath);
	if (!NewClass)
	{
		// Fall back to a soft class path resolve (covers BP generated _C classes).
		const FSoftClassPath SoftClass(ClassPath);
		NewClass = SoftClass.ResolveClass();
		if (!NewClass)
		{
			NewClass = LoadObject<UClass>(nullptr, *ClassPath);
		}
	}
	if (!NewClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not resolve class: %s"), *ClassPath));
	}

	// ContextData lives on the context-owner (root chooser); edit it there so the
	// view returned by GetContextData() reflects the change.
	UChooserTable* Owner = Table->GetContextOwner();
	if (!Owner)
	{
		return FMonolithActionResult::Error(TEXT("ChooserTable has no context owner"));
	}

	// Resolve the target context entry: explicit numeric index, else first class entry.
	int32 TargetIndex = INDEX_NONE;
	if (IndexStr.IsNumeric())
	{
		TargetIndex = FCString::Atoi(*IndexStr);
	}

	FContextObjectTypeClass* ClassEntry = nullptr;
	if (TargetIndex != INDEX_NONE)
	{
		if (!Owner->ContextData.IsValidIndex(TargetIndex))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("context index %d out of range (have %d entries)"), TargetIndex, Owner->ContextData.Num()));
		}
		ClassEntry = Owner->ContextData[TargetIndex].GetMutablePtr<FContextObjectTypeClass>();
		if (!ClassEntry)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("ContextData[%d] is not a class parameter (FContextObjectTypeClass)"), TargetIndex));
		}
	}
	else
	{
		for (int32 i = 0; i < Owner->ContextData.Num(); ++i)
		{
			if (FContextObjectTypeClass* Candidate = Owner->ContextData[i].GetMutablePtr<FContextObjectTypeClass>())
			{
				ClassEntry = Candidate;
				TargetIndex = i;
				break;
			}
		}
		if (!ClassEntry)
		{
			return FMonolithActionResult::Error(TEXT("No class-typed context parameter (FContextObjectTypeClass) found on this chooser"));
		}
	}

	const FString OldClass = ClassEntry->Class ? ClassEntry->Class->GetPathName() : TEXT("");
	ClassEntry->Class = NewClass;

	Owner->MarkPackageDirty();
	Owner->Compile(/*bForce=*/true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetNumberField(TEXT("context_index"), TargetIndex);
	Root->SetStringField(TEXT("old_class"), OldClass);
	Root->SetStringField(TEXT("new_class"), NewClass->GetPathName());
	return FMonolithActionResult::Success(Root);
}

// ===========================================================================
// set_result_asset_reference
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleSetResultAssetReference(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString NewAssetPath = Params->GetStringField(TEXT("asset_path_value"));

	double RowVal = 0.0;
	if (!Params->TryGetNumberField(TEXT("row_or_column"), RowVal))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row_or_column"));
	}
	const int32 Row = static_cast<int32>(RowVal);

	if (NewAssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path_value"));
	}

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

#if WITH_EDITORONLY_DATA
	if (!Table->ResultsStructs.IsValidIndex(Row))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d out of range (have %d rows)"), Row, Table->ResultsStructs.Num()));
	}

	FInstancedStruct& RowStruct = Table->ResultsStructs[Row];
	FString OldRef;

	if (FAssetChooser* Hard = RowStruct.GetMutablePtr<FAssetChooser>())
	{
		OldRef = Hard->Asset ? Hard->Asset->GetPathName() : TEXT("");
		UObject* Loaded = FMonolithAssetUtils::LoadAssetByPath(NewAssetPath);
		if (!Loaded)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Could not load asset for hard reference: %s"), *NewAssetPath));
		}
		Hard->Asset = Loaded;
	}
	else if (FSoftAssetChooser* Soft = RowStruct.GetMutablePtr<FSoftAssetChooser>())
	{
		OldRef = Soft->Asset.ToSoftObjectPath().ToString();
		Soft->Asset = TSoftObjectPtr<UObject>(FSoftObjectPath(NewAssetPath));
	}
	else
	{
		const UScriptStruct* SS = RowStruct.GetScriptStruct();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d is not an asset result (type: %s)"), Row, SS ? *SS->GetName() : TEXT("<null>")));
	}

	Table->MarkPackageDirty();
	Table->Compile(/*bForce=*/true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetNumberField(TEXT("row"), Row);
	Root->SetStringField(TEXT("old_asset"), OldRef);
	Root->SetStringField(TEXT("new_asset"), NewAssetPath);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("ResultsStructs is editor-only data; not available in this build"));
#endif
}

// ===========================================================================
// set_evaluate_chooser_result_reference
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleSetEvaluateChooserResultReference(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ChildChooserPath = Params->GetStringField(TEXT("child_chooser_path"));

	double RowVal = 0.0;
	if (!Params->TryGetNumberField(TEXT("row"), RowVal))
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: row"));
	}
	const int32 Row = static_cast<int32>(RowVal);

	if (ChildChooserPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: child_chooser_path"));
	}

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

#if WITH_EDITORONLY_DATA
	if (!Table->ResultsStructs.IsValidIndex(Row))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d out of range (have %d rows)"), Row, Table->ResultsStructs.Num()));
	}

	FInstancedStruct& RowStruct = Table->ResultsStructs[Row];
	FEvaluateChooser* Eval = RowStruct.GetMutablePtr<FEvaluateChooser>();
	if (!Eval)
	{
		const UScriptStruct* SS = RowStruct.GetScriptStruct();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("result row %d is not an EvaluateChooser row (type: %s); use set_result_asset_reference for asset rows"),
			Row, SS ? *SS->GetName() : TEXT("<null>")));
	}

	UChooserTable* ChildTable = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(ChildChooserPath);
	if (!ChildTable)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("child ChooserTable not found: %s"), *ChildChooserPath));
	}

	const FString OldRef = Eval->Chooser ? Eval->Chooser->GetPathName() : TEXT("");
	Eval->Chooser = ChildTable;

	Table->MarkPackageDirty();
	Table->Compile(/*bForce=*/true);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetNumberField(TEXT("row"), Row);
	Root->SetStringField(TEXT("old_child_chooser"), OldRef);
	Root->SetStringField(TEXT("new_child_chooser"), ChildTable->GetPathName());
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("ResultsStructs is editor-only data; not available in this build"));
#endif
}

// ===========================================================================
// validate_chooser
// ===========================================================================

FMonolithActionResult FMonolithChooserActions::HandleValidateChooser(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const FString ExpectedContextClass = Params->HasField(TEXT("expected_context_class"))
		? Params->GetStringField(TEXT("expected_context_class")) : FString();
	const FString ExpectedResultType = Params->HasField(TEXT("expected_result_type"))
		? Params->GetStringField(TEXT("expected_result_type")) : FString();

	UChooserTable* Table = FMonolithAssetUtils::LoadAssetByPath<UChooserTable>(AssetPath);
	if (!Table)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("ChooserTable not found: %s"), *AssetPath));
	}

	// Force-compile to surface validation state.
	Table->Compile(/*bForce=*/true);

	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bValid = true;

	// Expected result type check.
	if (!ExpectedResultType.IsEmpty())
	{
		EObjectChooserResultType Expected;
		if (!ParseResultType(ExpectedResultType, Expected))
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("unrecognized expected_result_type '%s'"), *ExpectedResultType)));
			bValid = false;
		}
		else if (Table->ResultType != Expected)
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result type mismatch: expected %s, got %s"),
				*ExpectedResultType, *ResultTypeToString(Table->ResultType))));
			bValid = false;
		}
	}

	// Expected context class check (any class-typed context entry matching).
	if (!ExpectedContextClass.IsEmpty())
	{
		bool bFound = false;
		const TConstArrayView<FInstancedStruct> ContextView = Table->GetContextData();
		for (const FInstancedStruct& Entry : ContextView)
		{
			if (const FContextObjectTypeClass* AsClass = Entry.GetPtr<FContextObjectTypeClass>())
			{
				if (AsClass->Class)
				{
					const FString CName = AsClass->Class->GetPathName();
					if (CName == ExpectedContextClass || AsClass->Class->GetName() == ExpectedContextClass)
					{
						bFound = true;
						break;
					}
				}
			}
		}
		if (!bFound)
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("no context class parameter matches expected '%s'"), *ExpectedContextClass)));
			bValid = false;
		}
	}

	// Null/stale result-row asset references.
	int32 NullRefs = 0;
#if WITH_EDITORONLY_DATA
	for (int32 r = 0; r < Table->ResultsStructs.Num(); ++r)
	{
		bool bIsNull = false;
		FString StructType;
		const FString AssetRef = GetRowAssetPath(Table->ResultsStructs[r], bIsNull, StructType);
		if (bIsNull)
		{
			++NullRefs;
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result row %d (%s) has a null asset reference"), r, *StructType)));
			bValid = false;
		}
		else if (!AssetRef.IsEmpty() && !FMonolithAssetUtils::AssetExists(AssetRef))
		{
			Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("result row %d references a missing asset: %s"), r, *AssetRef)));
			bValid = false;
		}
	}
#endif

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Table->GetPathName());
	Root->SetBoolField(TEXT("valid"), bValid);
	Root->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Root->SetStringField(TEXT("result_class"), Table->OutputObjectType ? Table->OutputObjectType->GetPathName() : TEXT(""));
	Root->SetNumberField(TEXT("null_ref_count"), NullRefs);
	Root->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Root);
}

#endif // WITH_CHOOSER
