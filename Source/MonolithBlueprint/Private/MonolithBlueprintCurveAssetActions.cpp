// SPDX-License-Identifier: MIT
// Standalone curve asset actions: create_curve_asset / get_curve_asset_keys / set_curve_asset_keys.
//
// Covers UCurveFloat (FloatCurve), UCurveVector (FloatCurves[3]) and
// UCurveLinearColor (FloatCurves[4]). Those members are FRichCurve UPROPERTYs
// that Python/Blueprint cannot edit, so keys are written through the FRichCurve
// API. The key JSON shape matches blueprint:set_curve_float_keys, plus a
// per-key "channel" for multi-channel curves. Game-thread only.

#include "MonolithBlueprintCurveAssetActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RichCurve.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithCurveAssetInternal
{
	struct FCurveAssetChannel
	{
		FString Name;
		FRichCurve* Curve = nullptr;
	};

	struct FCurveAssetParsedKey
	{
		int32 Channel = 0;
		float Time = 0.f;
		float Value = 0.f;
		ERichCurveInterpMode Interp = RCIM_Cubic;
		ERichCurveTangentMode TangentMode = RCTM_Auto;
		bool bHasArrive = false;
		bool bHasLeave = false;
		float Arrive = 0.f;
		float Leave = 0.f;
	};

	static const TArray<FString>& CurveAssetChannelNamesFor(const FString& CurveType)
	{
		static const TArray<FString> FloatNames  = { TEXT("value") };
		static const TArray<FString> VectorNames = { TEXT("x"), TEXT("y"), TEXT("z") };
		static const TArray<FString> ColorNames  = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };
		if (CurveType == TEXT("vector"))       { return VectorNames; }
		if (CurveType == TEXT("linear_color")) { return ColorNames; }
		return FloatNames;
	}

	/** Normalise a caller curve_type token. Returns empty on an unknown token. */
	static FString CurveAssetNormaliseType(const FString& InType)
	{
		const FString Lower = InType.ToLower();
		if (Lower == TEXT("float") || Lower == TEXT("curvefloat")) { return TEXT("float"); }
		if (Lower == TEXT("vector") || Lower == TEXT("curvevector")) { return TEXT("vector"); }
		if (Lower == TEXT("linear_color") || Lower == TEXT("linearcolor") || Lower == TEXT("color")
			|| Lower == TEXT("curvelinearcolor"))
		{
			return TEXT("linear_color");
		}
		return FString();
	}

	static UClass* CurveAssetClassForType(const FString& CurveType)
	{
		if (CurveType == TEXT("vector"))       { return UCurveVector::StaticClass(); }
		if (CurveType == TEXT("linear_color")) { return UCurveLinearColor::StaticClass(); }
		return UCurveFloat::StaticClass();
	}

	/** Collect the editable channels of a curve asset. Returns the curve_type token, or empty if unsupported. */
	static FString CurveAssetCollectChannels(UCurveBase* Curve, TArray<FCurveAssetChannel>& OutChannels)
	{
		OutChannels.Reset();
		if (UCurveFloat* Float = Cast<UCurveFloat>(Curve))
		{
			OutChannels.Add({ TEXT("value"), &Float->FloatCurve });
			return TEXT("float");
		}
		if (UCurveVector* Vector = Cast<UCurveVector>(Curve))
		{
			const TArray<FString>& Names = CurveAssetChannelNamesFor(TEXT("vector"));
			for (int32 Index = 0; Index < 3; ++Index)
			{
				OutChannels.Add({ Names[Index], &Vector->FloatCurves[Index] });
			}
			return TEXT("vector");
		}
		if (UCurveLinearColor* Color = Cast<UCurveLinearColor>(Curve))
		{
			const TArray<FString>& Names = CurveAssetChannelNamesFor(TEXT("linear_color"));
			for (int32 Index = 0; Index < 4; ++Index)
			{
				OutChannels.Add({ Names[Index], &Color->FloatCurves[Index] });
			}
			return TEXT("linear_color");
		}
		return FString();
	}

	static int32 CurveAssetFindChannel(const TArray<FString>& Names, const FString& InName)
	{
		const FString Lower = InName.ToLower();
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Names[Index] == Lower)
			{
				return Index;
			}
		}
		// Long-form aliases.
		const TCHAR* Short = nullptr;
		if (Lower == TEXT("red"))        { Short = TEXT("r"); }
		else if (Lower == TEXT("green")) { Short = TEXT("g"); }
		else if (Lower == TEXT("blue"))  { Short = TEXT("b"); }
		else if (Lower == TEXT("alpha")) { Short = TEXT("a"); }
		return Short ? Names.IndexOfByKey(FString(Short)) : INDEX_NONE;
	}

	static bool CurveAssetParseInterp(const FString& InStr, ERichCurveInterpMode& OutMode)
	{
		const FString Lower = InStr.ToLower();
		if (Lower == TEXT("linear"))   { OutMode = RCIM_Linear;   return true; }
		if (Lower == TEXT("constant")) { OutMode = RCIM_Constant; return true; }
		if (Lower == TEXT("cubic"))    { OutMode = RCIM_Cubic;    return true; }
		return false;
	}

	static FString CurveAssetInterpToString(ERichCurveInterpMode InMode)
	{
		switch (InMode)
		{
		case RCIM_Linear:   return TEXT("linear");
		case RCIM_Constant: return TEXT("constant");
		case RCIM_Cubic:    return TEXT("cubic");
		default:            return TEXT("none");
		}
	}

	static bool CurveAssetParseTangentMode(const FString& InStr, ERichCurveTangentMode& OutMode)
	{
		const FString Lower = InStr.ToLower();
		if (Lower == TEXT("auto"))                                     { OutMode = RCTM_Auto;      return true; }
		if (Lower == TEXT("smart_auto") || Lower == TEXT("smartauto")) { OutMode = RCTM_SmartAuto; return true; }
		if (Lower == TEXT("user"))                                     { OutMode = RCTM_User;      return true; }
		if (Lower == TEXT("break"))                                    { OutMode = RCTM_Break;     return true; }
		return false;
	}

	static FString CurveAssetTangentModeToString(ERichCurveTangentMode InMode)
	{
		switch (InMode)
		{
		case RCTM_Auto:      return TEXT("auto");
		case RCTM_SmartAuto: return TEXT("smart_auto");
		case RCTM_User:      return TEXT("user");
		case RCTM_Break:     return TEXT("break");
		default:             return TEXT("none");
		}
	}

	static FString CurveAssetExtrapToString(ERichCurveExtrapolation InExtrap)
	{
		if (const UEnum* Enum = StaticEnum<ERichCurveExtrapolation>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(InExtrap));
		}
		return FString::FromInt(static_cast<int32>(InExtrap));
	}

	static TSharedPtr<FJsonObject> CurveAssetSerializeChannel(const FCurveAssetChannel& Channel)
	{
		const FRichCurve& Rich = *Channel.Curve;

		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FRichCurveKey& Key : Rich.GetConstRefOfKeys())
		{
			TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
			K->SetNumberField(TEXT("time"), Key.Time);
			K->SetNumberField(TEXT("value"), Key.Value);
			K->SetStringField(TEXT("interp"), CurveAssetInterpToString(Key.InterpMode.GetValue()));
			K->SetStringField(TEXT("tangent_mode"), CurveAssetTangentModeToString(Key.TangentMode.GetValue()));
			K->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
			K->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
			Keys.Add(MakeShared<FJsonValueObject>(K));
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("channel"), Channel.Name);
		Obj->SetNumberField(TEXT("key_count"), Keys.Num());
		Obj->SetArrayField(TEXT("keys"), Keys);
		const float DefaultValue = Rich.GetDefaultValue();
		if (DefaultValue == MAX_flt)
		{
			Obj->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Obj->SetNumberField(TEXT("default_value"), DefaultValue);
		}
		Obj->SetStringField(TEXT("pre_infinity"), CurveAssetExtrapToString(Rich.PreInfinityExtrap.GetValue()));
		Obj->SetStringField(TEXT("post_infinity"), CurveAssetExtrapToString(Rich.PostInfinityExtrap.GetValue()));
		if (Keys.Num() > 0)
		{
			float MinTime = 0.f, MaxTime = 0.f, MinValue = 0.f, MaxValue = 0.f;
			Rich.GetTimeRange(MinTime, MaxTime);
			Rich.GetValueRange(MinValue, MaxValue);
			Obj->SetNumberField(TEXT("min_time"), MinTime);
			Obj->SetNumberField(TEXT("max_time"), MaxTime);
			Obj->SetNumberField(TEXT("min_value"), MinValue);
			Obj->SetNumberField(TEXT("max_value"), MaxValue);
		}
		return Obj;
	}

	/** Serialize every channel (or only OnlyChannel when >= 0). */
	static TSharedPtr<FJsonObject> CurveAssetSerialize(UCurveBase* Curve, const FString& AssetPath, int32 OnlyChannel = INDEX_NONE)
	{
		TArray<FCurveAssetChannel> Channels;
		const FString CurveType = CurveAssetCollectChannels(Curve, Channels);

		TArray<TSharedPtr<FJsonValue>> ChannelArray;
		for (int32 Index = 0; Index < Channels.Num(); ++Index)
		{
			if (OnlyChannel != INDEX_NONE && OnlyChannel != Index)
			{
				continue;
			}
			ChannelArray.Add(MakeShared<FJsonValueObject>(CurveAssetSerializeChannel(Channels[Index])));
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetStringField(TEXT("curve_type"), CurveType);
		Root->SetStringField(TEXT("class"), Curve->GetClass()->GetName());
		Root->SetNumberField(TEXT("channel_count"), ChannelArray.Num());
		Root->SetArrayField(TEXT("channels"), ChannelArray);
		if (const UCurveFloat* Float = Cast<UCurveFloat>(Curve))
		{
			Root->SetBoolField(TEXT("is_event_curve"), Float->bIsEventCurve);
		}
		return Root;
	}

	/**
	 * Validate a keys array before any curve is touched.
	 * DefaultChannel is used for keys without their own "channel"; INDEX_NONE means
	 * every key must name its channel (multi-channel curve, no channel param given).
	 */
	static bool CurveAssetParseKeys(const TArray<TSharedPtr<FJsonValue>>& KeysArray, const TArray<FString>& ChannelNames,
		int32 DefaultChannel, TArray<FCurveAssetParsedKey>& OutKeys, FString& OutError)
	{
		const FString ChannelList = FString::Join(ChannelNames, TEXT("|"));
		for (int32 Index = 0; Index < KeysArray.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& KeyVal = KeysArray[Index];
			const TSharedPtr<FJsonObject>* KeyObjPtr = nullptr;
			if (!KeyVal.IsValid() || !KeyVal->TryGetObject(KeyObjPtr) || !KeyObjPtr || !(*KeyObjPtr).IsValid())
			{
				OutError = FString::Printf(TEXT("keys[%d] is not an object"), Index);
				return false;
			}
			const TSharedPtr<FJsonObject>& KeyObj = *KeyObjPtr;

			FCurveAssetParsedKey Key;
			Key.Channel = DefaultChannel;
			FString ChannelStr;
			if (KeyObj->TryGetStringField(TEXT("channel"), ChannelStr) && !ChannelStr.IsEmpty())
			{
				Key.Channel = CurveAssetFindChannel(ChannelNames, ChannelStr);
				if (Key.Channel == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("keys[%d]: unknown channel '%s'. This curve accepts: %s"), Index, *ChannelStr, *ChannelList);
					return false;
				}
			}
			if (Key.Channel == INDEX_NONE)
			{
				OutError = FString::Printf(
					TEXT("keys[%d] has no 'channel'. Multi-channel curves need a per-key channel (%s) or a top-level channel param."),
					Index, *ChannelList);
				return false;
			}

			double Time = 0.0, Value = 0.0;
			if (!KeyObj->TryGetNumberField(TEXT("time"), Time) || !KeyObj->TryGetNumberField(TEXT("value"), Value))
			{
				OutError = FString::Printf(TEXT("keys[%d] needs numeric 'time' and 'value'"), Index);
				return false;
			}
			Key.Time = static_cast<float>(Time);
			Key.Value = static_cast<float>(Value);

			FString InterpStr;
			if (KeyObj->TryGetStringField(TEXT("interp"), InterpStr) || KeyObj->TryGetStringField(TEXT("interp_mode"), InterpStr))
			{
				if (!CurveAssetParseInterp(InterpStr, Key.Interp))
				{
					OutError = FString::Printf(
						TEXT("keys[%d]: unsupported interp '%s'. Use \"linear\", \"constant\", or \"cubic\"."), Index, *InterpStr);
					return false;
				}
			}

			FString TangentStr;
			if (KeyObj->TryGetStringField(TEXT("tangent_mode"), TangentStr) && !CurveAssetParseTangentMode(TangentStr, Key.TangentMode))
			{
				OutError = FString::Printf(
					TEXT("keys[%d]: unsupported tangent_mode '%s'. Use \"auto\", \"smart_auto\", \"user\", or \"break\"."), Index, *TangentStr);
				return false;
			}

			double Arrive = 0.0, Leave = 0.0;
			Key.bHasArrive = KeyObj->TryGetNumberField(TEXT("arrive_tangent"), Arrive) || KeyObj->TryGetNumberField(TEXT("arrive"), Arrive);
			Key.bHasLeave = KeyObj->TryGetNumberField(TEXT("leave_tangent"), Leave) || KeyObj->TryGetNumberField(TEXT("leave"), Leave);
			Key.Arrive = static_cast<float>(Arrive);
			Key.Leave = static_cast<float>(Leave);
			if ((Key.bHasArrive || Key.bHasLeave) && Key.TangentMode != RCTM_User && Key.TangentMode != RCTM_Break)
			{
				// Explicit tangents only stick on user/break keys; auto modes recompute them.
				Key.TangentMode = RCTM_User;
			}
			OutKeys.Add(Key);
		}
		return true;
	}

	struct FCurveAssetWriteStats
	{
		int32 KeysAdded = 0;
		int32 KeysUpdated = 0;
		TArray<FString> ChannelsWritten;
	};

	/** Apply pre-validated keys. Caller owns the transaction / Modify. */
	static FCurveAssetWriteStats CurveAssetApplyKeys(TArray<FCurveAssetChannel>& Channels, const TArray<FCurveAssetParsedKey>& Keys, bool bReplace)
	{
		FCurveAssetWriteStats Stats;

		TSet<int32> Touched;
		for (const FCurveAssetParsedKey& Key : Keys)
		{
			Touched.Add(Key.Channel);
		}
		if (bReplace)
		{
			// Replace clears only the channels being written; other channels keep their keys.
			for (const int32 ChannelIndex : Touched)
			{
				Channels[ChannelIndex].Curve->Reset();
			}
		}

		for (const FCurveAssetParsedKey& Key : Keys)
		{
			FRichCurve& Rich = *Channels[Key.Channel].Curve;
			FKeyHandle Handle = bReplace ? FKeyHandle::Invalid() : Rich.FindKey(Key.Time);
			if (Rich.IsKeyHandleValid(Handle))
			{
				Rich.SetKeyValue(Handle, Key.Value, /*bAutoSetTangents=*/false);
				++Stats.KeysUpdated;
			}
			else
			{
				Handle = Rich.AddKey(Key.Time, Key.Value);
				++Stats.KeysAdded;
			}
			Rich.SetKeyInterpMode(Handle, Key.Interp, /*bAutoSetTangents=*/false);
			Rich.SetKeyTangentMode(Handle, Key.TangentMode, /*bAutoSetTangents=*/false);

			FRichCurveKey& RawKey = Rich.GetKey(Handle);
			if (Key.bHasArrive)
			{
				RawKey.ArriveTangent = Key.Arrive;
			}
			if (Key.bHasLeave)
			{
				RawKey.LeaveTangent = Key.Leave;
			}
			else if (Key.bHasArrive && Key.TangentMode == RCTM_User)
			{
				// A user (unbroken) tangent is a single slope.
				RawKey.LeaveTangent = Key.Arrive;
			}
		}

		for (int32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
		{
			if (Touched.Contains(ChannelIndex))
			{
				// Recompute auto / smart-auto tangents; user and break keys are left as written.
				Channels[ChannelIndex].Curve->AutoSetTangents();
				Stats.ChannelsWritten.Add(Channels[ChannelIndex].Name);
			}
		}
		return Stats;
	}

	static TArray<TSharedPtr<FJsonValue>> CurveAssetStringArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	static UCurveBase* CurveAssetLoad(const FString& AssetPath, FString& OutError)
	{
		UObject* Obj = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (!Obj)
		{
			OutError = FString::Printf(TEXT("Curve asset not found: %s"), *AssetPath);
			return nullptr;
		}
		UCurveBase* Curve = Cast<UCurveBase>(Obj);
		TArray<FCurveAssetChannel> Channels;
		if (!Curve || CurveAssetCollectChannels(Curve, Channels).IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("'%s' is a %s, not a CurveFloat, CurveVector or CurveLinearColor asset"), *AssetPath, *Obj->GetClass()->GetName());
			return nullptr;
		}
		return Curve;
	}
}

// ============================================================
//  Registration
// ============================================================

void FMonolithBlueprintCurveAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	static const TCHAR* KeyShapeDesc =
		TEXT("Array of {time:number, value:number, channel?: string, interp?: \"linear\"|\"constant\"|\"cubic\" (default cubic), "
		     "tangent_mode?: \"auto\"|\"smart_auto\"|\"user\"|\"break\" (default auto), arrive_tangent?, leave_tangent?}. "
		     "channel is \"value\" for float curves, \"x\"|\"y\"|\"z\" for vector curves, \"r\"|\"g\"|\"b\"|\"a\" for linear color curves.");

	Registry.RegisterAction(TEXT("blueprint"), TEXT("create_curve_asset"),
		TEXT("Create a standalone curve asset: CurveFloat (curve_type=float), CurveVector (vector) or CurveLinearColor (linear_color), "
		     "optionally seeded with keys. Fails if an asset already exists at asset_path. Saves the new package by default. "
		     "Returns the per-channel keys."),
		FMonolithActionHandler::CreateStatic(&HandleCreateCurveAsset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path for the new asset, e.g. /Game/Data/Curves/CV_Example"))
			.Required(TEXT("curve_type"), TEXT("string"), TEXT("\"float\", \"vector\" or \"linear_color\""))
			.Optional(TEXT("keys"), TEXT("array"), KeyShapeDesc)
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Channel applied to keys that do not name their own (vector/linear_color only)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the new package (default true)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_curve_asset_keys"),
		TEXT("Read the keys of a CurveFloat, CurveVector or CurveLinearColor asset. Returns curve_type and "
		     "channels:[{channel, key_count, keys:[{time, value, interp, tangent_mode, arrive_tangent, leave_tangent}], "
		     "default_value, pre_infinity, post_infinity, min/max time and value}]. Pass channel to read one channel only."),
		FMonolithActionHandler::CreateStatic(&HandleGetCurveKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("CurveFloat / CurveVector / CurveLinearColor asset path"))
			.Optional(TEXT("channel"), TEXT("string"), TEXT("value | x|y|z | r|g|b|a. Omit to return every channel."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_curve_asset_keys"),
		TEXT("Write keys into a CurveFloat, CurveVector or CurveLinearColor asset. Each key targets a channel (per-key channel, "
		     "or the top-level channel param). replace=true (default) clears only the channels being written; replace=false "
		     "updates keys at matching times and adds the rest. All keys are validated before anything is written. "
		     "Auto / smart_auto tangents are recomputed; user / break keys keep the tangents you pass. Undoable; marks the package dirty."),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("CurveFloat / CurveVector / CurveLinearColor asset path"))
			.Required(TEXT("keys"), TEXT("array"), KeyShapeDesc)
			.Optional(TEXT("channel"), TEXT("string"), TEXT("Channel applied to keys that do not name their own. Not needed for float curves."))
			.Optional(TEXT("replace"), TEXT("boolean"), TEXT("Clear the written channels before writing (default true)"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after writing (default false)"), TEXT("false"))
			.Build());
}

// ============================================================
//  create_curve_asset
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveAssetActions::HandleCreateCurveAsset(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveAssetInternal;

	FString PackagePath = Params->GetStringField(TEXT("asset_path"));
	if (PackagePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}
	// Reject malformed paths before they reach CreatePackage, which asserts on them.
	if (const FString ValidationError = MonolithCore::ValidatePackagePath(PackagePath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	const FString CurveType = CurveAssetNormaliseType(Params->GetStringField(TEXT("curve_type")));
	if (CurveType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("curve_type must be \"float\", \"vector\" or \"linear_color\""));
	}
	const TArray<FString>& ChannelNames = CurveAssetChannelNamesFor(CurveType);

	int32 DefaultChannel = CurveType == TEXT("float") ? 0 : INDEX_NONE;
	FString ChannelParam;
	if (Params->TryGetStringField(TEXT("channel"), ChannelParam) && !ChannelParam.IsEmpty())
	{
		DefaultChannel = CurveAssetFindChannel(ChannelNames, ChannelParam);
		if (DefaultChannel == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown channel '%s' for a %s curve. Use: %s"),
				*ChannelParam, *CurveType, *FString::Join(ChannelNames, TEXT("|"))));
		}
	}

	// Validate keys before creating anything, so a bad key never leaves an empty asset behind.
	TArray<FCurveAssetParsedKey> Parsed;
	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (Params->TryGetArrayField(TEXT("keys"), KeysArray) && KeysArray)
	{
		FString KeyError;
		if (!CurveAssetParseKeys(*KeysArray, ChannelNames, DefaultChannel, Parsed, KeyError))
		{
			return FMonolithActionResult::Error(KeyError);
		}
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("asset_path has no asset name: %s"), *PackagePath));
	}
	const FString ObjectPath = PackagePath + TEXT(".") + AssetName;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (AR.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid() || FindObject<UObject>(nullptr, *ObjectPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("An asset already exists at '%s'. Delete it first or use blueprint:set_curve_asset_keys to edit it."), *PackagePath));
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
	}
	Package->FullyLoad();

	UCurveBase* Curve = NewObject<UCurveBase>(Package, CurveAssetClassForType(CurveType), FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!Curve)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create %s curve object: %s"), *CurveType, *AssetName));
	}

	TArray<FCurveAssetChannel> Channels;
	CurveAssetCollectChannels(Curve, Channels);
	const FCurveAssetWriteStats Stats = CurveAssetApplyKeys(Channels, Parsed, /*bReplace=*/true);

	Curve->PostEditChange();
	FAssetRegistryModule::AssetCreated(Curve);
	Package->MarkPackageDirty();

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);
	FString SaveError;
	const bool bSaved = bSave ? FMonolithAssetUtils::SaveAssetPackage(Package, &SaveError) : false;

	TSharedPtr<FJsonObject> Root = CurveAssetSerialize(Curve, PackagePath);
	Root->SetBoolField(TEXT("created"), true);
	Root->SetNumberField(TEXT("keys_added"), Stats.KeysAdded);
	Root->SetBoolField(TEXT("saved"), bSaved);
	if (bSave && !bSaved)
	{
		Root->SetStringField(TEXT("save_error"), SaveError);
	}
	return FMonolithActionResult::Success(Root);
}

// ============================================================
//  get_curve_asset_keys
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveAssetActions::HandleGetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveAssetInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadError;
	UCurveBase* Curve = CurveAssetLoad(AssetPath, LoadError);
	if (!Curve)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	int32 OnlyChannel = INDEX_NONE;
	FString ChannelParam;
	if (Params->TryGetStringField(TEXT("channel"), ChannelParam) && !ChannelParam.IsEmpty())
	{
		TArray<FCurveAssetChannel> Channels;
		const FString CurveType = CurveAssetCollectChannels(Curve, Channels);
		const TArray<FString>& ChannelNames = CurveAssetChannelNamesFor(CurveType);
		OnlyChannel = CurveAssetFindChannel(ChannelNames, ChannelParam);
		if (OnlyChannel == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown channel '%s' for a %s curve. Use: %s"),
				*ChannelParam, *CurveType, *FString::Join(ChannelNames, TEXT("|"))));
		}
	}

	return FMonolithActionResult::Success(CurveAssetSerialize(Curve, AssetPath, OnlyChannel));
}

// ============================================================
//  set_curve_asset_keys
// ============================================================

FMonolithActionResult FMonolithBlueprintCurveAssetActions::HandleSetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCurveAssetInternal;

	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: keys (array of {time, value, channel?, interp?, tangent_mode?})"));
	}

	FString LoadError;
	UCurveBase* Curve = CurveAssetLoad(AssetPath, LoadError);
	if (!Curve)
	{
		return FMonolithActionResult::Error(LoadError);
	}

	TArray<FCurveAssetChannel> Channels;
	const FString CurveType = CurveAssetCollectChannels(Curve, Channels);
	const TArray<FString>& ChannelNames = CurveAssetChannelNamesFor(CurveType);

	int32 DefaultChannel = Channels.Num() == 1 ? 0 : INDEX_NONE;
	FString ChannelParam;
	if (Params->TryGetStringField(TEXT("channel"), ChannelParam) && !ChannelParam.IsEmpty())
	{
		DefaultChannel = CurveAssetFindChannel(ChannelNames, ChannelParam);
		if (DefaultChannel == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown channel '%s' for a %s curve. Use: %s"),
				*ChannelParam, *CurveType, *FString::Join(ChannelNames, TEXT("|"))));
		}
	}

	TArray<FCurveAssetParsedKey> Parsed;
	FString KeyError;
	if (!CurveAssetParseKeys(*KeysArray, ChannelNames, DefaultChannel, Parsed, KeyError))
	{
		return FMonolithActionResult::Error(KeyError);
	}

	bool bReplace = true;
	Params->TryGetBoolField(TEXT("replace"), bReplace);

	Curve->SetFlags(RF_Transactional);
	FScopedTransaction Transaction(NSLOCTEXT(
		"MonolithBlueprintCurveAssetActions", "SetCurveAssetKeys", "Monolith Set Curve Keys"));
	Curve->Modify();

	const FCurveAssetWriteStats Stats = CurveAssetApplyKeys(Channels, Parsed, bReplace);

	Curve->PostEditChange();
	Curve->MarkPackageDirty();

	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	FString SaveError;
	const bool bSaved = bSave ? FMonolithAssetUtils::SaveAssetPackage(Curve->GetPackage(), &SaveError) : false;

	TSharedPtr<FJsonObject> Root = CurveAssetSerialize(Curve, AssetPath);
	Root->SetNumberField(TEXT("keys_added"), Stats.KeysAdded);
	Root->SetNumberField(TEXT("keys_updated"), Stats.KeysUpdated);
	Root->SetArrayField(TEXT("channels_written"), CurveAssetStringArray(Stats.ChannelsWritten));
	Root->SetBoolField(TEXT("replaced"), bReplace);
	Root->SetBoolField(TEXT("saved"), bSaved);
	if (bSave && !bSaved)
	{
		Root->SetStringField(TEXT("save_error"), SaveError);
	}
	return FMonolithActionResult::Success(Root);
}
