#include "MonolithChooserReadActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h" // FBlueprintTags::GeneratedClassPath
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

// NOTE: deliberately no "Chooser.h" include and no WITH_CHOOSER gate. Everything below
// reads UChooserTable through reflection only, so this file compiles and behaves
// identically with the optional Chooser plugin enabled or disabled. See the header.

namespace MonolithChooserRead
{
	// -----------------------------------------------------------------------
	// Bounds. Every traversal in this file is finite and every cutoff below is
	// surfaced in the response (truncated / depth_limited / complete fields).
	// -----------------------------------------------------------------------
	constexpr int32 MaxColumns                          = 512;    // columns serialized per response
	constexpr int32 MaxRowsPerResponse                  = 500;    // row-page ceiling
	constexpr int32 MaxTablesPerResponse                = 1000;   // discovery-page ceiling
	constexpr int32 MaxReferencesPerScan                = 4096;   // distinct references collected per scan
	constexpr int32 MaxReferencesPerResponse            = 1000;   // reference-page ceiling
	constexpr int32 MaxReferenceDepth                   = 12;     // struct/container nesting for the reference walk
	constexpr int32 MaxArrayElementsPerReferenceProperty = 4096;  // per-container element budget in the walk
	constexpr int32 MaxReferenceTraversalVisits         = 65536;  // GLOBAL property-visit budget for the walk
	constexpr int32 MaxResultPayloadsPerValidation      = 4096;   // result rows structurally validated
	constexpr int32 MaxSerializedDepth                  = 3;      // struct nesting for value readback
	constexpr int32 MaxSerializedFields                 = 128;    // reflected fields per struct (full mode)
	constexpr int32 MaxCompactSerializedFields          = 16;     // reflected fields per struct (compact mode)
	constexpr int32 MaxSerializedContainerElements      = 256;    // container elements per value (full mode)
	constexpr int32 MaxCompactContainerElements         = 8;      // container elements per value (compact mode)
	constexpr int32 MaxSerializedStringChars            = 4096;   // string/text/export-text payload length

	// JSON numbers are IEEE-754 doubles, so integers past 2^53-1 cannot round-trip.
	// Anything outside this range is emitted as a decimal STRING rather than silently
	// rounded to a wrong value.
	constexpr int64 MaxExactJsonInteger = 9007199254740991LL;

	// -----------------------------------------------------------------------
	// Parameter guards
	// -----------------------------------------------------------------------

	FMonolithActionResult InvalidParam(const TCHAR* Param, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("param"), Param);
		Data->SetStringField(TEXT("reason"), Reason);
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid parameter '%s': %s"), Param, *Reason),
			FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data);
	}

	/** Typed optional integer with an inclusive range. A wrong JSON type is an error, not a default. */
	bool ParseBoundedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
		int32 Default, int32 Min, int32 Max, int32& OutValue, FMonolithActionResult& OutError)
	{
		double Number = static_cast<double>(Default);
		if (Params.IsValid() && Params->HasField(Field))
		{
			const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
			if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number))
			{
				OutError = InvalidParam(Field, TEXT("expected an integer JSON number"));
				return false;
			}
		}
		if (!FMath::IsFinite(Number) || Number != FMath::TruncToDouble(Number)
			|| Number < static_cast<double>(Min) || Number > static_cast<double>(Max))
		{
			OutError = InvalidParam(Field,
				FString::Printf(TEXT("expected an integer in the range %d..%d"), Min, Max));
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	/** Typed optional boolean. A string "true" is a caller bug, not a truthy value. */
	bool ParseBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
		bool Default, bool& OutValue, FMonolithActionResult& OutError)
	{
		OutValue = Default;
		if (!Params.IsValid() || !Params->HasField(Field))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a boolean"));
			return false;
		}
		return true;
	}

	/** Typed optional string. */
	bool ParseString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
		bool bRequired, FString& OutValue, FMonolithActionResult& OutError)
	{
		OutValue.Reset();
		const TSharedPtr<FJsonValue> Value = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
		if (!Value.IsValid())
		{
			if (bRequired)
			{
				OutError = InvalidParam(Field, TEXT("expected a string"));
				return false;
			}
			return true;
		}
		if (Value->Type != EJson::String || !Value->TryGetString(OutValue))
		{
			OutError = InvalidParam(Field, TEXT("expected a string"));
			return false;
		}
		return true;
	}

	// -----------------------------------------------------------------------
	// Canonical path handling. This surface REJECTS aliases rather than repairing
	// them: a caller that gets a preflight answer for a path it did not ask about
	// cannot tell a hit from a near-miss.
	// -----------------------------------------------------------------------

	/** Shared spelling rules for both the filter and the asset path. */
	bool IsCanonicalSpelling(const FString& Input)
	{
		return !Input.Contains(TEXT("\\"))
			&& !Input.Contains(TEXT(":"))
			&& !Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			&& !Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase);
	}

	bool HasSurroundingWhitespace(const FString& Input)
	{
		FString Trimmed = Input;
		Trimmed.TrimStartAndEndInline();
		return Trimmed != Input;
	}

	/** Empty filter = whole project. Otherwise a mounted long package prefix, trailing slash removed. */
	bool NormalizePackageFilter(const FString& Input, FString& OutFilter, FMonolithActionResult& OutError)
	{
		OutFilter = Input;
		if (OutFilter.IsEmpty())
		{
			return true;
		}
		if (HasSurroundingWhitespace(Input))
		{
			OutError = InvalidParam(TEXT("path_filter"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (!IsCanonicalSpelling(Input) || !FPackageName::IsValidLongPackageName(Input))
		{
			OutError = InvalidParam(TEXT("path_filter"),
				TEXT("expected a canonical Unreal long package prefix beginning with a mounted root, for example /Game/Choosers"));
			return false;
		}
		OutFilter.RemoveFromEnd(TEXT("/"));
		return true;
	}

	/**
	 * Accept `/Game/F/CHT_A` or `/Game/F/CHT_A.CHT_A`; reject everything else, including a
	 * top-level object name that does not exactly match the package asset name.
	 */
	bool NormalizeAssetPath(const FString& Input, FString& OutPackagePath, FString& OutObjectPath,
		FMonolithActionResult& OutError)
	{
		OutPackagePath.Reset();
		OutObjectPath.Reset();
		if (Input.IsEmpty())
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("a non-empty path is required"));
			return false;
		}
		if (HasSurroundingWhitespace(Input))
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("leading or trailing whitespace is not allowed"));
			return false;
		}
		if (!Input.StartsWith(TEXT("/")) || !IsCanonicalSpelling(Input))
		{
			OutError = InvalidParam(TEXT("asset_path"),
				TEXT("expected a canonical Unreal package or top-level object path, not a filesystem, relative, or subobject path"));
			return false;
		}

		FString RequestedObjectName;
		int32 DotIndex = INDEX_NONE;
		if (Input.FindLastChar(TEXT('.'), DotIndex))
		{
			OutPackagePath = Input.Left(DotIndex);
			RequestedObjectName = Input.Mid(DotIndex + 1);
			if (RequestedObjectName.IsEmpty() || RequestedObjectName.Contains(TEXT(".")))
			{
				OutError = InvalidParam(TEXT("asset_path"), TEXT("the top-level object name is malformed"));
				return false;
			}
		}
		else
		{
			OutPackagePath = Input;
		}

		if (!FPackageName::IsValidLongPackageName(OutPackagePath))
		{
			OutError = InvalidParam(TEXT("asset_path"),
				TEXT("the package portion is not a valid mounted Unreal long package name"));
			return false;
		}

		const FString CanonicalName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (CanonicalName.IsEmpty())
		{
			OutError = InvalidParam(TEXT("asset_path"), TEXT("the package path has no asset name"));
			return false;
		}
		if (!RequestedObjectName.IsEmpty() && !RequestedObjectName.Equals(CanonicalName, ESearchCase::CaseSensitive))
		{
			OutError = InvalidParam(TEXT("asset_path"), FString::Printf(
				TEXT("top-level object name must exactly match the package asset name '%s'"), *CanonicalName));
			return false;
		}

		OutObjectPath = OutPackagePath + TEXT(".") + CanonicalName;
		return true;
	}

	FTopLevelAssetPath ChooserTableClassPath()
	{
		return FTopLevelAssetPath(TEXT("/Script/Chooser"), TEXT("ChooserTable"));
	}

	/** Null when the optional Chooser plugin is not linked/loaded — the availability signal. */
	UClass* FindChooserTableClass()
	{
		UClass* ChooserClass = FindObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"));
		if (!ChooserClass)
		{
			// Quiet: a missing /Script/Chooser module is an expected state here, not a fault.
			ChooserClass = LoadObject<UClass>(nullptr, TEXT("/Script/Chooser.ChooserTable"),
				FStringView(), LOAD_NoWarn | LOAD_Quiet);
		}
		return ChooserClass;
	}

	/** Resolve `asset_path` to an exactly-matching UChooserTable, or fill OutError. */
	UObject* LoadChooserFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath,
		FString& OutObjectPath, FMonolithActionResult& OutError)
	{
		FString RequestedPath;
		if (!ParseString(Params, TEXT("asset_path"), /*bRequired=*/true, RequestedPath, OutError)
			|| !NormalizeAssetPath(RequestedPath, OutPackagePath, OutObjectPath, OutError))
		{
			return nullptr;
		}

		UClass* ChooserClass = FindChooserTableClass();
		if (!ChooserClass)
		{
			OutError = FMonolithActionResult::Error(
				TEXT("ChooserTable class is unavailable; enable the engine Chooser plugin for asset readback"),
				FMonolithJsonUtils::ErrOptionalDepUnavailable);
			return nullptr;
		}

		UObject* Chooser = FMonolithAssetUtils::LoadAssetByPath(ChooserClass, OutObjectPath);
		if (!Chooser)
		{
			OutError = InvalidParam(TEXT("asset_path"), FString::Printf(
				TEXT("no ChooserTable resolves at the exact path '%s'"), *OutObjectPath));
			return nullptr;
		}

		// The shared loader intentionally resolves aliases and redirectors. This surface
		// must not: report exactly what the caller asked about, or nothing.
		const FString LoadedPath = Chooser->GetPathName();
		if (!LoadedPath.Equals(OutObjectPath, ESearchCase::CaseSensitive))
		{
			OutError = InvalidParam(TEXT("asset_path"), FString::Printf(
				TEXT("aliases, redirectors, and case-only variants are rejected (requested '%s', resolved '%s')"),
				*OutObjectPath, *LoadedPath));
			return nullptr;
		}
		return Chooser;
	}

	// -----------------------------------------------------------------------
	// Reflected array access on UChooserTable
	// -----------------------------------------------------------------------

	const FArrayProperty* FindArray(const UObject* Object, const TCHAR* PropertyName)
	{
		return Object ? FindFProperty<FArrayProperty>(Object->GetClass(), PropertyName) : nullptr;
	}

	/** False when the property does not exist at all (distinct from "exists and is empty"). */
	bool TryArrayNum(const UObject* Object, const TCHAR* PropertyName, int32& OutNum)
	{
		OutNum = 0;
		const FArrayProperty* ArrayProperty = FindArray(Object, PropertyName);
		if (!ArrayProperty)
		{
			return false;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		OutNum = Helper.Num();
		return true;
	}

	int32 ArrayNum(const UObject* Object, const TCHAR* PropertyName)
	{
		int32 Num = 0;
		TryArrayNum(Object, PropertyName, Num);
		return Num;
	}

	bool BoolArrayAt(const UObject* Object, const TCHAR* PropertyName, int32 Index)
	{
		const FArrayProperty* ArrayProperty = FindArray(Object, PropertyName);
		const FBoolProperty* Inner = ArrayProperty ? CastField<FBoolProperty>(ArrayProperty->Inner) : nullptr;
		if (!Inner)
		{
			return false;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		return Helper.IsValidIndex(Index) && Inner->GetPropertyValue(Helper.GetRawPtr(Index));
	}

	const FInstancedStruct* InstancedStructAt(const UObject* Object, const TCHAR* PropertyName, int32 Index)
	{
		const FArrayProperty* ArrayProperty = FindArray(Object, PropertyName);
		const FStructProperty* Inner = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!Inner || Inner->Struct != FInstancedStruct::StaticStruct())
		{
			return nullptr;
		}
		FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Object));
		return Helper.IsValidIndex(Index)
			? reinterpret_cast<const FInstancedStruct*>(Helper.GetRawPtr(Index))
			: nullptr;
	}

	/**
	 * The ACTIVE per-row value array of a column struct.
	 *
	 * UHT registers `TArray<bool> RowValues_DEPRECATED` under the name "RowValues" with
	 * CPF_Deprecated, and the live array is `RowValuesWithAny` (FBoolColumn) — so a naive
	 * FindFProperty("RowValues") reads the migrated-away array and reports zero cells for
	 * every bool column. Prefer a non-deprecated exact "RowValues", then "RowValuesWithAny",
	 * then the lowest-named non-deprecated RowValues* array for future column kinds.
	 */
	FArrayProperty* FindRowValuesProperty(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}
		if (FArrayProperty* Exact = FindFProperty<FArrayProperty>(Struct, TEXT("RowValues")))
		{
			if (!Exact->HasAnyPropertyFlags(CPF_Deprecated))
			{
				return Exact;
			}
		}
		if (FArrayProperty* WithAny = FindFProperty<FArrayProperty>(Struct, TEXT("RowValuesWithAny")))
		{
			return WithAny;
		}

		FArrayProperty* Best = nullptr;
		for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FArrayProperty* Candidate = CastField<FArrayProperty>(*It);
			if (!Candidate || !Candidate->GetName().Contains(TEXT("RowValues"))
				|| Candidate->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			if (!Best || Candidate->GetName() < Best->GetName())
			{
				Best = Candidate;
			}
		}
		return Best;
	}

	int32 RowValueNum(const FInstancedStruct& Column)
	{
		FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct());
		if (!RowValues || !Column.GetMemory())
		{
			return 0;
		}
		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		return Helper.Num();
	}

	// -----------------------------------------------------------------------
	// Bounded reflection serializer. Compact mode is used for column summaries;
	// full mode for row cells and fallback data.
	// -----------------------------------------------------------------------

	TSharedPtr<FJsonObject> StructFieldsToJson(const UStruct* Struct, const void* Memory, int32 MaxDepth, bool bCompact);
	TSharedPtr<FJsonObject> InstancedStructToJson(const FInstancedStruct& Instance, int32 MaxDepth, bool bCompact);
	TSharedPtr<FJsonValue> ValueToJson(const FProperty* Property, const void* Value, int32 MaxDepth, bool bCompact);

	/** Envelope shared by every container kind: total count, bounded page, explicit cutoff. */
	TSharedPtr<FJsonValue> MakeContainerJson(int32 TotalCount, const TCHAR* ItemsField,
		const TArray<TSharedPtr<FJsonValue>>& Items, bool bDepthLimited)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), TotalCount);
		if (bDepthLimited)
		{
			Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
			Result->SetBoolField(TEXT("depth_limited"), true);
		}
		Result->SetArrayField(ItemsField, Items);
		if (!bDepthLimited && Items.Num() < TotalCount)
		{
			Result->SetNumberField(TEXT("truncated_after"), Items.Num());
		}
		return MakeShared<FJsonValueObject>(Result);
	}

	TSharedPtr<FJsonValue> BoundedString(const FString& Value, const TCHAR* Serialization)
	{
		if (Value.Len() <= MaxSerializedStringChars)
		{
			return MakeShared<FJsonValueString>(Value);
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("serialization"), Serialization);
		Result->SetStringField(TEXT("value"), Value.Left(MaxSerializedStringChars));
		Result->SetNumberField(TEXT("original_char_count"), Value.Len());
		Result->SetNumberField(TEXT("truncated_after"), MaxSerializedStringChars);
		return MakeShared<FJsonValueObject>(Result);
	}

	/** Integers past 2^53-1 are emitted as decimal strings; a rounded number would be a lie. */
	TSharedPtr<FJsonValue> ExactIntegerJson(int64 Raw)
	{
		if (Raw > MaxExactJsonInteger || Raw < -MaxExactJsonInteger)
		{
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Raw));
		}
		return MakeShared<FJsonValueNumber>(static_cast<double>(Raw));
	}

	/** Fixed-size C arrays (ArrayDim > 1) — element zero is not the whole property. */
	TSharedPtr<FJsonValue> FixedArrayToJson(const FProperty* Property, const void* Container, int32 MaxDepth, bool bCompact)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		if (MaxDepth > 0)
		{
			const int32 Limit = bCompact ? MaxCompactContainerElements : MaxSerializedContainerElements;
			const int32 Count = FMath::Min(Property->ArrayDim, Limit);
			Items.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Items.Add(ValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Container, Index), MaxDepth - 1, bCompact));
			}
		}
		return MakeContainerJson(Property->ArrayDim, TEXT("items"), Items, MaxDepth <= 0);
	}

	TSharedPtr<FJsonValue> ValueToJson(const FProperty* Property, const void* Value, int32 MaxDepth, bool bCompact)
	{
		if (!Property || !Value)
		{
			return MakeShared<FJsonValueNull>();
		}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(Value));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			const FString Name = EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetNameStringByValue(Raw) : FString();
			if (!Name.IsEmpty())
			{
				return MakeShared<FJsonValueString>(Name);
			}
			return ExactIntegerJson(Raw);
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 Raw = ByteProperty->GetPropertyValue(Value);
			const FString Name = ByteProperty->Enum ? ByteProperty->Enum->GetNameStringByValue(Raw) : FString();
			if (!Name.IsEmpty())
			{
				return MakeShared<FJsonValueString>(Name);
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(Raw));
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (!NumericProperty->IsInteger())
			{
				return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(Value));
			}
			// Only uint64 can exceed int64; every narrower unsigned type fits losslessly.
			if (CastField<FUInt64Property>(Property))
			{
				const uint64 Raw = NumericProperty->GetUnsignedIntPropertyValue(Value);
				if (Raw > static_cast<uint64>(MaxExactJsonInteger))
				{
					return MakeShared<FJsonValueString>(FString::Printf(TEXT("%llu"), Raw));
				}
				return MakeShared<FJsonValueNumber>(static_cast<double>(Raw));
			}
			return ExactIntegerJson(NumericProperty->GetSignedIntPropertyValue(Value));
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(Value).ToString());
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return BoundedString(StringProperty->GetPropertyValue(Value), TEXT("string"));
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return BoundedString(TextProperty->GetPropertyValue(Value).ToString(), TEXT("text"));
		}

		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			return MakeShared<FJsonValueString>(SoftProperty->GetPropertyValue(Value).ToSoftObjectPath().ToString());
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value);
			return MakeShared<FJsonValueString>(Object ? Object->GetPathName() : TEXT("None"));
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const bool bInstanced = StructProperty->Struct == FInstancedStruct::StaticStruct();
			if (MaxDepth <= 0)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("serialization"), TEXT("depth_limit"));
				Result->SetBoolField(TEXT("depth_limited"), true);
				Result->SetStringField(TEXT("property_type"), Property->GetCPPType());
				const UScriptStruct* Reported = StructProperty->Struct;
				if (bInstanced)
				{
					const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Value);
					Result->SetBoolField(TEXT("valid"), Instance.IsValid());
					Reported = Instance.GetScriptStruct();
				}
				Result->SetStringField(TEXT("script_struct"), Reported ? Reported->GetPathName() : FString());
				return MakeShared<FJsonValueObject>(Result);
			}
			if (bInstanced)
			{
				return MakeShared<FJsonValueObject>(
					InstancedStructToJson(*static_cast<const FInstancedStruct*>(Value), MaxDepth - 1, bCompact));
			}
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("script_struct"),
				StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
			Result->SetObjectField(TEXT("fields"),
				StructFieldsToJson(StructProperty->Struct, Value, MaxDepth - 1, bCompact));
			return MakeShared<FJsonValueObject>(Result);
		}

		const int32 ContainerLimit = bCompact ? MaxCompactContainerElements : MaxSerializedContainerElements;
		const bool bDepthLimited = MaxDepth <= 0;

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Items;
			if (!bDepthLimited)
			{
				const int32 Count = FMath::Min(Helper.Num(), ContainerLimit);
				Items.Reserve(Count);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Items.Add(ValueToJson(ArrayProperty->Inner, Helper.GetRawPtr(Index), MaxDepth - 1, bCompact));
				}
			}
			return MakeContainerJson(Helper.Num(), TEXT("items"), Items, bDepthLimited);
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Items;
			if (!bDepthLimited)
			{
				Items.Reserve(FMath::Min(Helper.Num(), ContainerLimit));
				for (int32 Index = 0; Index < Helper.GetMaxIndex() && Items.Num() < ContainerLimit; ++Index)
				{
					if (Helper.IsValidIndex(Index))
					{
						Items.Add(ValueToJson(SetProperty->ElementProp, Helper.GetElementPtr(Index), MaxDepth - 1, bCompact));
					}
				}
			}
			return MakeContainerJson(Helper.Num(), TEXT("items"), Items, bDepthLimited);
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, Value);
			TArray<TSharedPtr<FJsonValue>> Entries;
			if (!bDepthLimited)
			{
				Entries.Reserve(FMath::Min(Helper.Num(), ContainerLimit));
				for (int32 Index = 0; Index < Helper.GetMaxIndex() && Entries.Num() < ContainerLimit; ++Index)
				{
					if (!Helper.IsValidIndex(Index))
					{
						continue;
					}
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetField(TEXT("key"), ValueToJson(MapProperty->KeyProp, Helper.GetKeyPtr(Index), MaxDepth - 1, bCompact));
					Entry->SetField(TEXT("value"), ValueToJson(MapProperty->ValueProp, Helper.GetValuePtr(Index), MaxDepth - 1, bCompact));
					Entries.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			return MakeContainerJson(Helper.Num(), TEXT("entries"), Entries, bDepthLimited);
		}

		// Anything without a first-class JSON shape (delegates, field paths, ...) round-trips
		// through ExportText rather than silently disappearing from the readback.
		FString Exported;
		Property->ExportTextItem_Direct(Exported, Value, nullptr, nullptr, PPF_None);
		return BoundedString(Exported, TEXT("export_text"));
	}

	TSharedPtr<FJsonObject> StructFieldsToJson(const UStruct* Struct, const void* Memory, int32 MaxDepth, bool bCompact)
	{
		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (!Struct || !Memory)
		{
			return Fields;
		}

		const int32 FieldLimit = bCompact ? MaxCompactSerializedFields : MaxSerializedFields;
		int32 FieldCount = 0;
		// Deprecated properties hold pre-migration values (see FindRowValuesProperty); emitting
		// them alongside the live ones would present two contradictory answers for one cell.
		for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			if (FieldCount >= FieldLimit)
			{
				Fields->SetNumberField(TEXT("__truncated_after"), FieldCount);
				break;
			}
			Fields->SetField(Property->GetName(), Property->ArrayDim > 1
				? FixedArrayToJson(Property, Memory, MaxDepth, bCompact)
				: ValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Memory), MaxDepth, bCompact));
			++FieldCount;
		}
		return Fields;
	}

	TSharedPtr<FJsonObject> InstancedStructToJson(const FInstancedStruct& Instance, int32 MaxDepth, bool bCompact)
	{
		const UScriptStruct* Struct = Instance.GetScriptStruct();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), Instance.IsValid());
		Result->SetStringField(TEXT("script_struct"), Struct ? Struct->GetPathName() : FString());
		Result->SetStringField(TEXT("type"), Struct ? Struct->GetName() : FString());
		Result->SetStringField(TEXT("display_name"), Struct ? Struct->GetDisplayNameText().ToString() : FString());
		Result->SetObjectField(TEXT("fields"), StructFieldsToJson(Struct, Instance.GetMemory(), MaxDepth, bCompact));
		return Result;
	}

	// -----------------------------------------------------------------------
	// Column / row projection
	// -----------------------------------------------------------------------

	bool ColumnBool(const FInstancedStruct& Column, const TCHAR* PropertyName)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		const FBoolProperty* Property = Struct ? FindFProperty<FBoolProperty>(Struct, PropertyName) : nullptr;
		return Property && Column.GetMemory()
			&& Property->GetPropertyValue(Property->ContainerPtrToValuePtr<void>(Column.GetMemory()));
	}

	/**
	 * Output (writer) columns vs input (filter) columns. Every FOutput*Column is named
	 * Output* and carries a FallbackValue property; either signal alone is enough, so a
	 * future column kind that follows only one convention is still classified.
	 */
	bool IsOutputColumn(const FInstancedStruct& Column)
	{
		const UScriptStruct* Struct = Column.GetScriptStruct();
		return Struct
			&& (Struct->GetName().StartsWith(TEXT("Output"))
				|| FindFProperty<FProperty>(Struct, TEXT("FallbackValue")) != nullptr);
	}

	TSharedPtr<FJsonValue> RowValueAt(const FInstancedStruct& Column, int32 RowIndex)
	{
		FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct());
		if (!RowValues || !Column.GetMemory())
		{
			return MakeShared<FJsonValueNull>();
		}
		FScriptArrayHelper Helper(RowValues, RowValues->ContainerPtrToValuePtr<void>(Column.GetMemory()));
		if (!Helper.IsValidIndex(RowIndex))
		{
			return MakeShared<FJsonValueNull>();
		}
		return ValueToJson(RowValues->Inner, Helper.GetRawPtr(RowIndex), MaxSerializedDepth, /*bCompact=*/false);
	}

	TSharedPtr<FJsonObject> ColumnSummary(const FInstancedStruct& Column, int32 Index)
	{
		TSharedPtr<FJsonObject> Result = InstancedStructToJson(Column, /*MaxDepth=*/2, /*bCompact=*/true);
		Result->SetNumberField(TEXT("index"), Index);
		Result->SetBoolField(TEXT("is_output"), IsOutputColumn(Column));
		Result->SetBoolField(TEXT("is_disabled"), ColumnBool(Column, TEXT("bDisabled")));

		FArrayProperty* RowValues = FindRowValuesProperty(Column.GetScriptStruct());
		Result->SetStringField(TEXT("row_values_property"), RowValues ? RowValues->GetName() : FString());
		Result->SetStringField(TEXT("row_values_type"),
			RowValues && RowValues->Inner ? RowValues->Inner->GetCPPType() : FString());
		Result->SetNumberField(TEXT("row_value_count"), RowValues ? RowValueNum(Column) : 0);
		return Result;
	}

	/**
	 * ResultsStructs owns the editor row model. CookedResults is derived data that can be
	 * stale after an editor mutation, so it is only a fallback for a cooked/stripped asset —
	 * taking a max across the two would turn staleness into phantom rows.
	 */
	int32 ChooserRowCount(const UObject* Chooser)
	{
		int32 RowCount = 0;
		if (TryArrayNum(Chooser, TEXT("ResultsStructs"), RowCount))
		{
			return RowCount;
		}
		return TryArrayNum(Chooser, TEXT("CookedResults"), RowCount) ? RowCount : 0;
	}

	/** Context parameters live on the ROOT chooser (UChooserTable::GetContextData follows RootChooser). */
	int32 ContextEntryCount(const UObject* Chooser)
	{
		if (!Chooser)
		{
			return 0;
		}
		const UObject* ContextOwner = Chooser;
		if (const FObjectPropertyBase* RootProperty =
			FindFProperty<FObjectPropertyBase>(Chooser->GetClass(), TEXT("RootChooser")))
		{
			if (const UObject* Root = RootProperty->GetObjectPropertyValue_InContainer(Chooser))
			{
				ContextOwner = Root;
			}
		}
		return ArrayNum(ContextOwner, TEXT("ContextData"));
	}

	TArray<TSharedPtr<FJsonValue>> SerializeColumns(const UObject* Chooser, int32& OutTotal, bool& OutTruncated)
	{
		OutTotal = ArrayNum(Chooser, TEXT("ColumnsStructs"));
		const int32 Count = FMath::Min(OutTotal, MaxColumns);
		OutTruncated = Count < OutTotal;

		TArray<TSharedPtr<FJsonValue>> Columns;
		Columns.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (const FInstancedStruct* Column = InstancedStructAt(Chooser, TEXT("ColumnsStructs"), Index))
			{
				Columns.Add(MakeShared<FJsonValueObject>(ColumnSummary(*Column, Index)));
			}
		}
		return Columns;
	}

	TArray<TSharedPtr<FJsonValue>> SerializeRows(const UObject* Chooser, int32 StartRow, int32 Limit,
		int32& OutTotalColumns, int32& OutCellsPerRow)
	{
		const int32 RowCount = ChooserRowCount(Chooser);
		OutTotalColumns = ArrayNum(Chooser, TEXT("ColumnsStructs"));
		OutCellsPerRow = FMath::Min(OutTotalColumns, MaxColumns);
		const int32 EndRow = FMath::Min(RowCount, StartRow + Limit);

		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(FMath::Max(0, EndRow - StartRow));
		for (int32 RowIndex = StartRow; RowIndex < EndRow; ++RowIndex)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("row_index"), RowIndex);
			Row->SetBoolField(TEXT("disabled"), BoolArrayAt(Chooser, TEXT("DisabledRows"), RowIndex));

			if (const FInstancedStruct* Result = InstancedStructAt(Chooser, TEXT("ResultsStructs"), RowIndex))
			{
				Row->SetObjectField(TEXT("result"), InstancedStructToJson(*Result, MaxSerializedDepth, /*bCompact=*/false));
			}
			else
			{
				Row->SetField(TEXT("result"), MakeShared<FJsonValueNull>());
			}

			TArray<TSharedPtr<FJsonValue>> Cells;
			Cells.Reserve(OutCellsPerRow);
			for (int32 ColumnIndex = 0; ColumnIndex < OutCellsPerRow; ++ColumnIndex)
			{
				const FInstancedStruct* Column = InstancedStructAt(Chooser, TEXT("ColumnsStructs"), ColumnIndex);
				if (!Column)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
				Cell->SetNumberField(TEXT("column_index"), ColumnIndex);
				Cell->SetStringField(TEXT("column_type"),
					Column->GetScriptStruct() ? Column->GetScriptStruct()->GetName() : FString());
				Cell->SetBoolField(TEXT("is_output"), IsOutputColumn(*Column));
				Cell->SetField(TEXT("value"), RowValueAt(*Column, RowIndex));
				Cells.Add(MakeShared<FJsonValueObject>(Cell));
			}
			Row->SetArrayField(TEXT("cells"), Cells);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	// -----------------------------------------------------------------------
	// Reference walk
	// -----------------------------------------------------------------------

	struct FReferenceScan
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		TSet<FString> Seen;
		TMap<FString, bool> ExistsCache;

		/** Set when a count budget stopped collection. Aborts the remaining walk. */
		bool bTruncated = false;
		/** Set when the depth budget blocked a descent. Sibling traversal continues, but the graph is unproven. */
		bool bDepthLimited = false;
		/** Global property/element budget: per-container caps alone do not bound nested products. */
		int32 Visits = 0;

		bool IsComplete() const { return !bTruncated && !bDepthLimited; }
	};

	/**
	 * Exact existence evidence for a soft path. Deliberately strict: a loaded but empty
	 * UPackage shell, an on-disk package whose export was deleted, and a package-only
	 * registry row for a requested subobject are all NON-existence.
	 */
	bool AssetExistsForSoftPath(const FSoftObjectPath& Path, bool bSoftClass, FReferenceScan& Scan)
	{
		const FString AssetPath = Path.GetAssetPathString();
		if (AssetPath.IsEmpty())
		{
			return false;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		const bool bScriptPackage = PackageName.StartsWith(TEXT("/Script/"));
		if (!bScriptPackage && !FPackageName::IsValidLongPackageName(PackageName))
		{
			return false;
		}

		const FString ExactPath = Path.ToString();
		const FString CacheKey = ExactPath + (bSoftClass ? TEXT("|soft_class") : TEXT("|soft_object"));
		if (const bool* Cached = Scan.ExistsCache.Find(CacheKey))
		{
			return *Cached;
		}

		bool bExists = false;
		if (const UObject* Resolved = Path.ResolveObject())
		{
			bExists = IsValid(Resolved) && Resolved->GetPathName().Equals(ExactPath, ESearchCase::CaseSensitive);
		}

		// A /Script/ reference is an exact export reference. If it did not resolve, the module
		// is absent or the export is gone; a package-level guess would be evidence of nothing.
		if (!bExists && !bScriptPackage && Path.GetSubPathString().IsEmpty())
		{
			// AssetRegistry does an exact object-path lookup. Package residency or on-disk
			// package existence is NOT sufficient — a deleted export leaves both behind.
			bExists = FMonolithAssetUtils::AssetExists(AssetPath);

			if (!bExists && bSoftClass)
			{
				// A Blueprint-generated class export (/Game/F/BP_F.BP_F_C) has no registry row of
				// its own; the registry indexes the owning Blueprint. Accept it only for a soft
				// CLASS property, and only when the Blueprint's GeneratedClassPath tag matches
				// the requested export exactly.
				FString PackageOnly;
				FString ObjectName;
				if (AssetPath.Split(TEXT("."), &PackageOnly, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
					&& ObjectName.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive)
					&& ObjectName.Len() > 2)
				{
					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					const FAssetData BlueprintData =
						Registry.GetAssetByObjectPath(FSoftObjectPath(PackageOnly + TEXT(".") + ObjectName.LeftChop(2)));
					FString GeneratedClassExportPath;
					if (BlueprintData.IsValid()
						&& BlueprintData.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassExportPath))
					{
						bExists = FPackageName::ExportTextPathToObjectPath(GeneratedClassExportPath)
							.Equals(AssetPath, ESearchCase::CaseSensitive);
					}
				}
			}
		}

		Scan.ExistsCache.Add(CacheKey, bExists);
		return bExists;
	}

	void AddReference(FReferenceScan& Scan, const FString& Source, const FString& Path,
		const FString& ClassName, bool bLoaded, bool bSoft, bool bExists)
	{
		if (Path.IsEmpty())
		{
			return;
		}
		const FString Key = Source + TEXT("|") + Path;
		if (Scan.Seen.Contains(Key))
		{
			return;
		}
		if (Scan.Values.Num() >= MaxReferencesPerScan)
		{
			Scan.bTruncated = true;
			return;
		}
		Scan.Seen.Add(Key);

		TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
		Reference->SetStringField(TEXT("source"), Source);
		Reference->SetStringField(TEXT("path"), Path);
		Reference->SetStringField(TEXT("class"), ClassName);
		Reference->SetBoolField(TEXT("loaded"), bLoaded);
		Reference->SetBoolField(TEXT("soft"), bSoft);
		Reference->SetBoolField(TEXT("exists"), bExists);
		if (bSoft)
		{
			const FSoftObjectPath SoftPath(Path);
			Reference->SetStringField(TEXT("asset_path"), SoftPath.GetAssetPathString());
			Reference->SetStringField(TEXT("sub_path"), SoftPath.GetSubPathString());
		}
		Scan.Values.Add(MakeShared<FJsonValueObject>(Reference));
	}

	void WalkStruct(const UStruct* Struct, const void* Memory, const FString& Source, FReferenceScan& Scan, int32 Depth);

	void WalkProperty(const FProperty* Property, const void* Value, const FString& Source, FReferenceScan& Scan, int32 Depth)
	{
		if (!Property || !Value || Scan.bTruncated)
		{
			return;
		}
		if (Depth < 0)
		{
			// Not an abort: siblings still get walked. But the graph was not fully visited,
			// so validate_chooser_table must not claim complete=true over it.
			Scan.bDepthLimited = true;
			return;
		}
		if (Scan.Visits >= MaxReferenceTraversalVisits)
		{
			Scan.bTruncated = true;
			return;
		}
		++Scan.Visits;

		if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath Path = SoftProperty->GetPropertyValue(Value).ToSoftObjectPath();
			if (Path.IsValid())
			{
				AddReference(Scan, Source, Path.ToString(),
					SoftProperty->PropertyClass ? SoftProperty->PropertyClass->GetName() : TEXT("soft_object"),
					Path.ResolveObject() != nullptr, /*bSoft=*/true,
					AssetExistsForSoftPath(Path, CastField<FSoftClassProperty>(Property) != nullptr, Scan));
			}
			return;
		}
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (const UObject* Object = ObjectProperty->GetObjectPropertyValue(Value))
			{
				AddReference(Scan, Source, Object->GetPathName(), Object->GetClass()->GetName(),
					/*bLoaded=*/true, /*bSoft=*/false, /*bExists=*/true);
			}
			return;
		}
		if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
		{
			// TScriptInterface is not an FObjectPropertyBase, so its object is invisible to
			// the branch above and to every container case below.
			if (const UObject* Object = InterfaceProperty->GetPropertyValue(Value).GetObject())
			{
				AddReference(Scan, Source, Object->GetPathName(), Object->GetClass()->GetName(),
					/*bLoaded=*/true, /*bSoft=*/false, /*bExists=*/true);
			}
			return;
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == FInstancedStruct::StaticStruct())
			{
				const FInstancedStruct& Instance = *static_cast<const FInstancedStruct*>(Value);
				if (Instance.IsValid())
				{
					WalkStruct(Instance.GetScriptStruct(), Instance.GetMemory(), Source, Scan, Depth - 1);
				}
				return;
			}
			WalkStruct(StructProperty->Struct, Value, Source, Scan, Depth - 1);
			return;
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, Value);
			const int32 Count = FMath::Min(Helper.Num(), MaxArrayElementsPerReferenceProperty);
			for (int32 Index = 0; Index < Count && !Scan.bTruncated; ++Index)
			{
				WalkProperty(ArrayProperty->Inner, Helper.GetRawPtr(Index),
					FString::Printf(TEXT("%s[%d]"), *Source, Index), Scan, Depth - 1);
			}
			Scan.bTruncated |= Count < Helper.Num();
			return;
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, Value);
			int32 Visited = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && !Scan.bTruncated; ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (++Visited > MaxArrayElementsPerReferenceProperty)
				{
					Scan.bTruncated = true;
					break;
				}
				WalkProperty(SetProperty->ElementProp, Helper.GetElementPtr(Index), Source + TEXT("{}"), Scan, Depth - 1);
			}
			return;
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, Value);
			int32 Visited = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && !Scan.bTruncated; ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (++Visited > MaxArrayElementsPerReferenceProperty)
				{
					Scan.bTruncated = true;
					break;
				}
				const FString EntrySource = Source + TEXT("{}");
				WalkProperty(MapProperty->KeyProp, Helper.GetKeyPtr(Index), EntrySource + TEXT(".key"), Scan, Depth - 1);
				WalkProperty(MapProperty->ValueProp, Helper.GetValuePtr(Index), EntrySource + TEXT(".value"), Scan, Depth - 1);
			}
		}
	}

	void WalkStruct(const UStruct* Struct, const void* Memory, const FString& Source, FReferenceScan& Scan, int32 Depth)
	{
		if (!Struct || !Memory || Scan.bTruncated)
		{
			return;
		}
		if (Depth < 0)
		{
			Scan.bDepthLimited = true;
			return;
		}
		// Deprecated properties can retain legacy paths the active struct no longer uses;
		// walking them produces false unresolved-reference findings after a migration.
		for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It && !Scan.bTruncated; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}
			const FString PropertySource = Source + TEXT(".") + Property->GetName();
			if (Property->ArrayDim <= 1)
			{
				WalkProperty(Property, Property->ContainerPtrToValuePtr<void>(Memory), PropertySource, Scan, Depth);
				continue;
			}
			// A reflected fixed-size C array stores ArrayDim adjacent elements; visiting only
			// element zero silently drops the rest.
			for (int32 Element = 0; Element < Property->ArrayDim && !Scan.bTruncated; ++Element)
			{
				WalkProperty(Property, Property->ContainerPtrToValuePtr<void>(Memory, Element),
					FString::Printf(TEXT("%s[%d]"), *PropertySource, Element), Scan, Depth);
			}
		}
	}

	/** Stable ordering so paginated reference pages are reproducible across calls. */
	FReferenceScan CollectReferences(const UObject* Chooser)
	{
		FReferenceScan Scan;
		WalkStruct(Chooser ? Chooser->GetClass() : nullptr, Chooser, TEXT("chooser"), Scan, MaxReferenceDepth);

		auto SortKey = [](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				return FString();
			}
			FString Path, Source, ClassName;
			(*Object)->TryGetStringField(TEXT("path"), Path);
			(*Object)->TryGetStringField(TEXT("source"), Source);
			(*Object)->TryGetStringField(TEXT("class"), ClassName);
			return Path + TEXT("\n") + Source + TEXT("\n") + ClassName;
		};
		Scan.Values.Sort([&SortKey](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return SortKey(Left) < SortKey(Right);
		});
		return Scan;
	}

	// -----------------------------------------------------------------------
	// Structural validation (no compile, no mutation)
	// -----------------------------------------------------------------------

	struct FValidationReport
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;

		void Add(const TCHAR* Severity, const TCHAR* Code, const FString& Message)
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			if (FCString::Strcmp(Severity, TEXT("error")) == 0)
			{
				++ErrorCount;
			}
			else
			{
				++WarningCount;
			}
		}
	};

	/** The target property a known result struct must have set, or null for an unknown kind. */
	const TCHAR* ResultTargetProperty(const UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return nullptr;
		}
		const FName Name = Struct->GetFName();
		if (Name == TEXT("AssetChooser") || Name == TEXT("SoftAssetChooser")) { return TEXT("Asset"); }
		if (Name == TEXT("EvaluateChooser") || Name == TEXT("NestedChooser")) { return TEXT("Chooser"); }
		if (Name == TEXT("ClassChooser"))                                     { return TEXT("Class"); }
		return nullptr;
	}

	bool HasResultTarget(const FInstancedStruct& Result, const TCHAR* TargetProperty)
	{
		const UScriptStruct* Struct = Result.GetScriptStruct();
		if (!Struct || !Result.GetMemory() || !TargetProperty)
		{
			return false;
		}
		if (const FSoftObjectProperty* Soft = FindFProperty<FSoftObjectProperty>(Struct, TargetProperty))
		{
			return Soft->GetPropertyValue(Soft->ContainerPtrToValuePtr<void>(Result.GetMemory()))
				.ToSoftObjectPath().IsValid();
		}
		if (const FObjectPropertyBase* Hard = FindFProperty<FObjectPropertyBase>(Struct, TargetProperty))
		{
			return Hard->GetObjectPropertyValue(Hard->ContainerPtrToValuePtr<void>(Result.GetMemory())) != nullptr;
		}
		return false;
	}

	/** Returns false when the row budget stopped short of validating every result. */
	bool ValidateResultPayloads(const UObject* Chooser, int32 ResultCount, FValidationReport& Report)
	{
		const int32 Count = FMath::Min(ResultCount, MaxResultPayloadsPerValidation);
		for (int32 RowIndex = 0; RowIndex < Count; ++RowIndex)
		{
			const FInstancedStruct* Result = InstancedStructAt(Chooser, TEXT("ResultsStructs"), RowIndex);
			if (!Result || !Result->IsValid())
			{
				Report.Add(TEXT("error"), TEXT("invalid_result_struct"),
					FString::Printf(TEXT("Result row %d has no valid reflected struct."), RowIndex));
				continue;
			}
			const UScriptStruct* Struct = Result->GetScriptStruct();
			const TCHAR* TargetProperty = ResultTargetProperty(Struct);
			if (TargetProperty && !HasResultTarget(*Result, TargetProperty))
			{
				Report.Add(TEXT("error"), TEXT("invalid_result_payload"),
					FString::Printf(TEXT("Result row %d (%s) has no valid '%s' target."),
						RowIndex, Struct ? *Struct->GetName() : TEXT("Unknown"), TargetProperty));
			}
		}
		if (Count < ResultCount)
		{
			Report.Add(TEXT("error"), TEXT("result_validation_truncated"),
				FString::Printf(TEXT("Result payload validation stopped after %d of %d rows."), Count, ResultCount));
			return false;
		}
		return true;
	}

	// -----------------------------------------------------------------------
	// AssetRegistry discovery
	// -----------------------------------------------------------------------

	TArray<TSharedPtr<FJsonValue>> QueryChooserAssets(const FString& PathFilter, int32 Offset, int32 Limit, int32& OutTotal)
	{
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(ChooserTableClassPath(), Assets, /*bSearchSubClasses=*/true);

		// Exact package-prefix match on a path BOUNDARY: /Game/Choosers must not match
		// /Game/ChoosersOld. Sorted so pagination is stable between calls.
		Assets.RemoveAll([&PathFilter](const FAssetData& Asset)
		{
			if (PathFilter.IsEmpty())
			{
				return false;
			}
			const FString PackagePath = Asset.PackageName.ToString();
			return !PackagePath.Equals(PathFilter, ESearchCase::CaseSensitive)
				&& !PackagePath.StartsWith(PathFilter + TEXT("/"), ESearchCase::CaseSensitive);
		});
		Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});
		OutTotal = Assets.Num();

		TArray<TSharedPtr<FJsonValue>> Page;
		const int32 End = Offset >= OutTotal ? OutTotal : FMath::Min(OutTotal, Offset + Limit);
		Page.Reserve(FMath::Max(0, End - Offset));
		for (int32 Index = Offset; Index < End; ++Index)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("asset_path"), Assets[Index].GetObjectPathString());
			Entry->SetStringField(TEXT("package_path"), Assets[Index].PackageName.ToString());
			Entry->SetStringField(TEXT("name"), Assets[Index].AssetName.ToString());
			Page.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Page;
	}

	/** Common preamble on every asset-backed response. */
	void SetAssetIdentity(const TSharedPtr<FJsonObject>& Result, const FString& ObjectPath, const FString& PackagePath)
	{
		Result->SetStringField(TEXT("asset_path"), ObjectPath);
		Result->SetStringField(TEXT("package_path"), PackagePath);
	}

	/** Common completeness block for any response carrying a reference scan. */
	void SetScanCompleteness(const TSharedPtr<FJsonObject>& Result, const FReferenceScan& Scan, const TCHAR* Prefix)
	{
		Result->SetBoolField(FString(Prefix) + TEXT("truncated"), Scan.bTruncated);
		Result->SetBoolField(FString(Prefix) + TEXT("depth_limited"), Scan.bDepthLimited);
		Result->SetNumberField(FString(Prefix) + TEXT("visits"), Scan.Visits);
		Result->SetNumberField(FString(Prefix) + TEXT("visit_limit"), MaxReferenceTraversalVisits);
		Result->SetBoolField(FString(Prefix) + TEXT("complete"), Scan.IsComplete());
	}
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithChooserReadActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// asset_path / path_filter deliberately use the UNTAGGED string schema rather than
	// RequiredAssetPath: the dispatcher silently rewrites backslashes on AssetPath-kind
	// params, and this surface must reject a non-canonical spelling instead of answering
	// a preflight question about a path the caller did not ask about.

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_tables"),
		TEXT("List UChooserTable assets from the AssetRegistry with exact package-prefix filtering and bounded, stable pagination. Reports total + has_more so a truncated page is never mistaken for the whole project. Registry-only: answers with metadata (available=false) even when the optional Chooser plugin is disabled."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Canonical mounted long package prefix, e.g. /Game/Choosers. Matched on a path boundary, so /Game/Choosers never matches /Game/ChoosersOld. Aliases and backslash spellings are rejected, not repaired."))
			.Optional(TEXT("offset"), TEXT("number"), TEXT("Zero-based result offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum tables to return (1-1000)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("get_chooser_table"),
		TEXT("Bounded UChooserTable summary: row/column/result/cooked-result/disabled-row/nested/context counts, bounded column summaries, the fallback result, and the bounded reference scan with its completeness fields. Set include_rows=true for a bounded first page of rows + cells. Strictly read-only — never compiles, saves or dirties the package (use chooser:validate_chooser for the compile-oriented pass)."),
		FMonolithActionHandler::CreateStatic(&HandleGetChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact UChooserTable package path or matching top-level object path. Aliases, redirectors and case-only variants are rejected."))
			.Optional(TEXT("include_rows"), TEXT("bool"), TEXT("Include a bounded row + cell readback"), TEXT("false"))
			.Optional(TEXT("row_limit"), TEXT("number"), TEXT("Maximum rows when include_rows=true (1-500)"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_columns"),
		TEXT("Reflected UChooserTable column inventory: per-column struct type, output/input role, disabled state, the ACTIVE row-value property name + element type, and its row-value count (deprecated migrated-away arrays are ignored). Bounded at 512 columns with an explicit truncated flag."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserColumns),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact UChooserTable package path or matching top-level object path"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_rows"),
		TEXT("Bounded page of UChooserTable result rows with per-column reflected cell values. Reports row_cells_per_row vs column_count so a partially-cellsed wide row is never mistaken for a complete one, plus has_more for the row page itself."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserRows),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact UChooserTable package path or matching top-level object path"))
			.Optional(TEXT("start_row"), TEXT("number"), TEXT("Zero-based first row"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("list_chooser_references"),
		TEXT("Stable, deduplicated page of every hard and soft reference reachable from a UChooserTable by reflection, each with its source property chain and EXACT existence evidence (a loaded empty package shell or a deleted export reads as exists=false). Reports scan_truncated / scan_depth_limited / scan_complete so an unfinished walk is never silently reported as clean."),
		FMonolithActionHandler::CreateStatic(&HandleListChooserReferences),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact UChooserTable package path or matching top-level object path"))
			.Optional(TEXT("offset"), TEXT("number"), TEXT("Zero-based reference offset"), TEXT("0"))
			.Optional(TEXT("limit"), TEXT("number"), TEXT("Maximum references to return (1-1000)"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("chooser"), TEXT("validate_chooser_table"),
		TEXT("Non-mutating structural preflight for a UChooserTable: per-column row-value alignment against the row count, ResultsStructs / DisabledRows alignment, result-target validity per result kind, and unresolved soft references. DISTINCT from chooser:validate_chooser, which runs Compile(true) — this one never compiles, mutates, saves or dirties anything. 'complete' is false whenever any bound stopped the check short."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChooserTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact UChooserTable package path or matching top-level object path"))
			.Build());
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserTables(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FMonolithActionResult Error;
	FString RequestedFilter;
	FString PathFilter;
	if (!ParseString(Params, TEXT("path_filter"), /*bRequired=*/false, RequestedFilter, Error)
		|| !NormalizePackageFilter(RequestedFilter, PathFilter, Error))
	{
		return Error;
	}

	int32 Offset = 0;
	int32 Limit = 200;
	if (!ParseBoundedInt(Params, TEXT("offset"), 0, 0, MAX_int32, Offset, Error)
		|| !ParseBoundedInt(Params, TEXT("limit"), 200, 1, MaxTablesPerResponse, Limit, Error))
	{
		return Error;
	}

	int32 Total = 0;
	TArray<TSharedPtr<FJsonValue>> Tables = QueryChooserAssets(PathFilter, Offset, Limit, Total);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("available"), FindChooserTableClass() != nullptr);
	Result->SetStringField(TEXT("class_path"), ChooserTableClassPath().ToString());
	Result->SetStringField(TEXT("path_filter"), PathFilter);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("count"), Tables.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetBoolField(TEXT("has_more"), Offset + Tables.Num() < Total);
	Result->SetArrayField(TEXT("tables"), Tables);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleGetChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FMonolithActionResult Error;
	bool bIncludeRows = false;
	int32 RowLimit = 50;
	if (!ParseBool(Params, TEXT("include_rows"), false, bIncludeRows, Error)
		|| !ParseBoundedInt(Params, TEXT("row_limit"), 50, 1, MaxRowsPerResponse, RowLimit, Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = ChooserRowCount(Chooser);
	int32 ColumnCount = 0;
	bool bColumnsTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Columns = SerializeColumns(Chooser, ColumnCount, bColumnsTruncated);
	const FReferenceScan References = CollectReferences(Chooser);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	SetAssetIdentity(Result, ObjectPath, PackagePath);
	Result->SetStringField(TEXT("class"), Chooser->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetNumberField(TEXT("result_count"), ArrayNum(Chooser, TEXT("ResultsStructs")));
	Result->SetNumberField(TEXT("cooked_result_count"), ArrayNum(Chooser, TEXT("CookedResults")));
	Result->SetNumberField(TEXT("disabled_row_count"), ArrayNum(Chooser, TEXT("DisabledRows")));
	Result->SetNumberField(TEXT("nested_chooser_count"), ArrayNum(Chooser, TEXT("NestedChoosers")));
	Result->SetNumberField(TEXT("nested_object_count"), ArrayNum(Chooser, TEXT("NestedObjects")));
	Result->SetNumberField(TEXT("context_entry_count"), ContextEntryCount(Chooser));
	Result->SetArrayField(TEXT("columns"), Columns);
	Result->SetBoolField(TEXT("columns_truncated"), bColumnsTruncated);
	Result->SetArrayField(TEXT("references"), References.Values);
	SetScanCompleteness(Result, References, TEXT("references_"));

	if (FStructProperty* Fallback = FindFProperty<FStructProperty>(Chooser->GetClass(), TEXT("FallbackResult")))
	{
		Result->SetField(TEXT("fallback_result"), ValueToJson(Fallback,
			Fallback->ContainerPtrToValuePtr<void>(Chooser), MaxSerializedDepth, /*bCompact=*/false));
	}

	if (bIncludeRows)
	{
		int32 RowTotalColumns = 0;
		int32 RowCellsPerRow = 0;
		TArray<TSharedPtr<FJsonValue>> Rows =
			SerializeRows(Chooser, 0, FMath::Min(RowCount, RowLimit), RowTotalColumns, RowCellsPerRow);
		Result->SetArrayField(TEXT("rows"), Rows);
		Result->SetNumberField(TEXT("rows_returned"), Rows.Num());
		Result->SetBoolField(TEXT("rows_truncated"), Rows.Num() < RowCount);
		Result->SetNumberField(TEXT("row_cells_per_row"), RowCellsPerRow);
		Result->SetBoolField(TEXT("row_cells_truncated"), RowCellsPerRow < RowTotalColumns);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserColumns(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FString PackagePath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	int32 Total = 0;
	bool bTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Columns = SerializeColumns(Chooser, Total, bTruncated);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	SetAssetIdentity(Result, ObjectPath, PackagePath);
	Result->SetArrayField(TEXT("columns"), Columns);
	Result->SetNumberField(TEXT("count"), Columns.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("column_limit"), MaxColumns);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserRows(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FMonolithActionResult Error;
	int32 StartRow = 0;
	int32 Limit = 100;
	if (!ParseBoundedInt(Params, TEXT("start_row"), 0, 0, MAX_int32, StartRow, Error)
		|| !ParseBoundedInt(Params, TEXT("limit"), 100, 1, MaxRowsPerResponse, Limit, Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = ChooserRowCount(Chooser);
	if (StartRow > RowCount)
	{
		return InvalidParam(TEXT("start_row"),
			FString::Printf(TEXT("expected a row index in the range 0..%d"), RowCount));
	}

	int32 TotalColumns = 0;
	int32 CellsPerRow = 0;
	TArray<TSharedPtr<FJsonValue>> Rows = SerializeRows(Chooser, StartRow, Limit, TotalColumns, CellsPerRow);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	SetAssetIdentity(Result, ObjectPath, PackagePath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("start_row"), StartRow);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetBoolField(TEXT("has_more"), StartRow + Rows.Num() < RowCount);
	// Each row is capped at MaxColumns cells, so a wide table returns a PARTIAL
	// predicate/output set. Report that rather than let it read as a complete row.
	Result->SetNumberField(TEXT("column_count"), TotalColumns);
	Result->SetNumberField(TEXT("row_cells_per_row"), CellsPerRow);
	Result->SetBoolField(TEXT("row_cells_truncated"), CellsPerRow < TotalColumns);
	Result->SetArrayField(TEXT("rows"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleListChooserReferences(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FMonolithActionResult Error;
	int32 Offset = 0;
	int32 Limit = 200;
	if (!ParseBoundedInt(Params, TEXT("offset"), 0, 0, MaxReferencesPerScan, Offset, Error)
		|| !ParseBoundedInt(Params, TEXT("limit"), 200, 1, MaxReferencesPerResponse, Limit, Error))
	{
		return Error;
	}

	FString PackagePath;
	FString ObjectPath;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const FReferenceScan Scan = CollectReferences(Chooser);
	const int32 Total = Scan.Values.Num();
	TArray<TSharedPtr<FJsonValue>> Page;
	if (Offset < Total)
	{
		const int32 End = FMath::Min(Total, Offset + Limit);
		Page.Append(&Scan.Values[Offset], End - Offset);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	SetAssetIdentity(Result, ObjectPath, PackagePath);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("count"), Page.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("scan_limit"), MaxReferencesPerScan);
	Result->SetBoolField(TEXT("has_more"), Offset + Page.Num() < Total);
	SetScanCompleteness(Result, Scan, TEXT("scan_"));
	Result->SetArrayField(TEXT("references"), Page);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithChooserReadActions::HandleValidateChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithChooserRead;

	FString PackagePath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UObject* Chooser = LoadChooserFromParams(Params, PackagePath, ObjectPath, Error);
	if (!Chooser)
	{
		return Error;
	}

	const int32 RowCount = ChooserRowCount(Chooser);
	const int32 ColumnCount = ArrayNum(Chooser, TEXT("ColumnsStructs"));
	FValidationReport Report;

	if (RowCount == 0)
	{
		Report.Add(TEXT("warning"), TEXT("empty_rows"), TEXT("ChooserTable has no rows."));
	}
	if (ColumnCount == 0)
	{
		Report.Add(TEXT("warning"), TEXT("empty_columns"), TEXT("ChooserTable has no columns."));
	}
	if (ColumnCount > MaxColumns)
	{
		Report.Add(TEXT("error"), TEXT("column_limit_exceeded"), FString::Printf(
			TEXT("ChooserTable has %d columns; bounded validation supports at most %d."), ColumnCount, MaxColumns));
	}

	// Row model: ResultsStructs and DisabledRows must both be exactly RowCount long.
	int32 ResultsCount = 0;
	const bool bHasEditorResults = TryArrayNum(Chooser, TEXT("ResultsStructs"), ResultsCount);
	if (bHasEditorResults && ResultsCount != RowCount)
	{
		Report.Add(TEXT("error"), TEXT("results_row_count_mismatch"), FString::Printf(
			TEXT("ResultsStructs has %d entries for %d reflected rows."), ResultsCount, RowCount));
	}
	const bool bResultsComplete = !bHasEditorResults || ValidateResultPayloads(Chooser, ResultsCount, Report);

	int32 DisabledCount = 0;
	if (TryArrayNum(Chooser, TEXT("DisabledRows"), DisabledCount) && DisabledCount != RowCount)
	{
		Report.Add(TEXT("error"), TEXT("disabled_row_count_mismatch"), FString::Printf(
			TEXT("DisabledRows has %d entries for %d reflected rows."), DisabledCount, RowCount));
	}

	// Column model: every column's ACTIVE per-row array must be exactly RowCount long.
	const int32 ColumnsToValidate = FMath::Min(ColumnCount, MaxColumns);
	for (int32 ColumnIndex = 0; ColumnIndex < ColumnsToValidate; ++ColumnIndex)
	{
		const FInstancedStruct* Column = InstancedStructAt(Chooser, TEXT("ColumnsStructs"), ColumnIndex);
		if (!Column || !Column->IsValid())
		{
			Report.Add(TEXT("error"), TEXT("invalid_column_struct"),
				FString::Printf(TEXT("Column %d has no valid reflected struct."), ColumnIndex));
			continue;
		}
		const UScriptStruct* Struct = Column->GetScriptStruct();
		if (!FindRowValuesProperty(Struct))
		{
			Report.Add(TEXT("warning"), TEXT("column_has_no_reflected_row_values"), FString::Printf(
				TEXT("Column %d (%s) exposes no reflected row-value array."),
				ColumnIndex, Struct ? *Struct->GetName() : TEXT("Unknown")));
			continue;
		}
		const int32 ValueCount = RowValueNum(*Column);
		if (ValueCount != RowCount)
		{
			Report.Add(TEXT("error"), TEXT("column_row_count_mismatch"), FString::Printf(
				TEXT("Column %d has %d row values for %d reflected rows."), ColumnIndex, ValueCount, RowCount));
		}
	}

	// Reference model: a soft target that does not exist is a broken table.
	const FReferenceScan References = CollectReferences(Chooser);
	for (const TSharedPtr<FJsonValue>& Value : References.Values)
	{
		const TSharedPtr<FJsonObject>* Reference = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Reference) || !Reference || !Reference->IsValid())
		{
			continue;
		}
		bool bSoft = false;
		bool bExists = true;
		(*Reference)->TryGetBoolField(TEXT("soft"), bSoft);
		(*Reference)->TryGetBoolField(TEXT("exists"), bExists);
		if (bSoft && !bExists)
		{
			FString Path;
			(*Reference)->TryGetStringField(TEXT("path"), Path);
			Report.Add(TEXT("error"), TEXT("unresolved_soft_reference"),
				FString::Printf(TEXT("Soft-reference asset does not resolve: %s"), *Path));
		}
	}
	if (References.bTruncated)
	{
		Report.Add(TEXT("error"), TEXT("reference_scan_truncated"),
			TEXT("Reference validation exceeded its bounded scan limit; validity cannot be proven."));
	}
	if (References.bDepthLimited)
	{
		Report.Add(TEXT("error"), TEXT("reference_scan_depth_limited"),
			TEXT("Reference validation hit its nesting-depth budget; part of the reflected graph was never checked, so validity cannot be proven."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	SetAssetIdentity(Result, ObjectPath, PackagePath);
	Result->SetNumberField(TEXT("row_count"), RowCount);
	Result->SetNumberField(TEXT("column_count"), ColumnCount);
	Result->SetArrayField(TEXT("issues"), Report.Issues);
	Result->SetNumberField(TEXT("issue_count"), Report.Issues.Num());
	Result->SetNumberField(TEXT("error_count"), Report.ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), Report.WarningCount);
	SetScanCompleteness(Result, References, TEXT("reference_scan_"));
	// complete=false whenever ANY bound stopped the check short, independent of validity.
	Result->SetBoolField(TEXT("complete"),
		References.IsComplete() && bResultsComplete && ColumnCount <= MaxColumns);
	Result->SetBoolField(TEXT("valid"), Report.ErrorCount == 0);
	return FMonolithActionResult::Success(Result);
}
