#include "MonolithGASInputAssetCommon.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace MonolithInput
{

FMonolithActionResult InvalidParam(const TCHAR* Field, const FString& Detail)
{
	return FMonolithActionResult::Error(
		FString::Printf(TEXT("Invalid parameter '%s': %s"), Field, *Detail), -32602);
}

bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	return Params.IsValid() && Params->HasField(Field);
}

bool ReadBoundedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue,
	int32 MinValue, int32 MaxValue, int32& OutValue, FMonolithActionResult& OutError)
{
	double Number = static_cast<double>(DefaultValue);
	if (HasParam(Params, Field))
	{
		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number))
		{
			OutError = InvalidParam(Field, TEXT("expected an integer JSON number"));
			return false;
		}
	}

	if (!FMath::IsFinite(Number) || Number != FMath::TruncToDouble(Number)
		|| Number < static_cast<double>(MinValue) || Number > static_cast<double>(MaxValue))
	{
		OutError = InvalidParam(Field,
			FString::Printf(TEXT("expected an integer in the range %d..%d"), MinValue, MaxValue));
		return false;
	}

	OutValue = static_cast<int32>(Number);
	return true;
}

bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefaultValue,
	bool& bOutValue, FMonolithActionResult& OutError)
{
	bOutValue = bDefaultValue;
	if (!HasParam(Params, Field))
	{
		return true;
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::Boolean || !Value->TryGetBool(bOutValue))
	{
		OutError = InvalidParam(Field, TEXT("expected a boolean"));
		return false;
	}
	return true;
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
	FString& OutValue, FMonolithActionResult& OutError)
{
	if (!HasParam(Params, Field))
	{
		OutError = InvalidParam(Field, TEXT("field is required"));
		return false;
	}
	if (!ReadOptionalString(Params, Field, FString(), OutValue, OutError))
	{
		return false;
	}
	if (OutValue.IsEmpty())
	{
		OutError = InvalidParam(Field, TEXT("expected a non-empty string"));
		return false;
	}
	return true;
}

bool ReadOptionalString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const FString& DefaultValue,
	FString& OutValue, FMonolithActionResult& OutError)
{
	OutValue = DefaultValue;
	if (!HasParam(Params, Field))
	{
		return true;
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
	if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(OutValue))
	{
		OutError = InvalidParam(Field, TEXT("expected a string"));
		return false;
	}
	return true;
}

bool ParsePackagePrefix(const FString& Input, FString& OutPrefix, FMonolithActionResult& OutError)
{
	FString Trimmed = Input;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed != Input)
	{
		OutError = InvalidParam(TEXT("path"), TEXT("leading or trailing whitespace is not allowed"));
		return false;
	}
	if (Input.Contains(TEXT("\\")) || Input.Contains(TEXT(":")) || Input.Contains(TEXT("."))
		|| Input.EndsWith(TEXT("/")))
	{
		OutError = InvalidParam(TEXT("path"),
			TEXT("expected a canonical Unreal package prefix such as /Game/Input"));
		return false;
	}
	if (!FPackageName::IsValidLongPackageName(Input))
	{
		OutError = InvalidParam(TEXT("path"), TEXT("expected a valid mounted long package prefix"));
		return false;
	}

	OutPrefix = Input;
	return true;
}

bool ParseObjectPath(const FString& Input, const TCHAR* Field, FString& OutObjectPath, FMonolithActionResult& OutError)
{
	FString Trimmed = Input;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed != Input)
	{
		OutError = InvalidParam(Field, TEXT("leading or trailing whitespace is not allowed"));
		return false;
	}
	if (Input.Contains(TEXT("\\")) || Input.Contains(TEXT(":"))
		|| Input.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
		|| Input.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
	{
		OutError = InvalidParam(Field, TEXT("expected a canonical Unreal package or top-level object path"));
		return false;
	}

	FString PackagePath;
	FString ObjectName;
	const bool bHasObjectSuffix = Input.Split(TEXT("."), &PackagePath, &ObjectName);
	if (!bHasObjectSuffix)
	{
		PackagePath = Input;
	}
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		OutError = InvalidParam(Field, TEXT("expected a valid mounted long package name"));
		return false;
	}

	const FString PackageLeaf = FPackageName::GetLongPackageAssetName(PackagePath);
	if (PackageLeaf.IsEmpty())
	{
		OutError = InvalidParam(Field, TEXT("asset name is missing"));
		return false;
	}
	// Only top-level objects are addressable here: a mismatched leaf is a caller mistake,
	// not something to silently coerce into "the asset that happens to live at this package".
	if (bHasObjectSuffix && ObjectName != PackageLeaf)
	{
		OutError = InvalidParam(Field, FString::Printf(
			TEXT("object name '%s' must match package leaf '%s'"), *ObjectName, *PackageLeaf));
		return false;
	}

	OutObjectPath = PackagePath + TEXT(".") + PackageLeaf;
	if (!FPackageName::IsValidObjectPath(OutObjectPath))
	{
		OutError = InvalidParam(Field, TEXT("expected a valid top-level object path"));
		return false;
	}
	return true;
}

UObject* LoadExact(UClass* ExpectedClass, const FString& Input, const TCHAR* Field,
	const TCHAR* ExpectedTypeName, FMonolithActionResult& OutError)
{
	FString ObjectPath;
	if (!ParseObjectPath(Input, Field, ObjectPath, OutError))
	{
		return nullptr;
	}

	UObject* Object = FSoftObjectPath(ObjectPath).TryLoad();
	if (!Object)
	{
		OutError = FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *ObjectPath));
		return nullptr;
	}
	if (!ExpectedClass || !Object->IsA(ExpectedClass))
	{
		OutError = FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset '%s' is %s, expected %s"), *ObjectPath, *Object->GetClass()->GetName(), ExpectedTypeName));
		return nullptr;
	}
	return Object;
}

void QueryAssets(UClass* AssetClass, const FString& PackagePrefix, TArray<FAssetData>& OutAssets)
{
	FARFilter Filter;
	Filter.ClassPaths.Add(AssetClass->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*PackagePrefix));
	Filter.bRecursivePaths = true;

	FAssetRegistryModule& Module =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Module.Get().GetAssets(Filter, OutAssets);

	// Sorted so offset/limit paging is stable across calls.
	OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetObjectPathString() < B.GetObjectPathString();
	});
}

TSharedPtr<FJsonObject> DescribeInstanced(const UObject* Object)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("class"), Object ? Object->GetClass()->GetName() : TEXT("None"));
	Json->SetStringField(TEXT("class_path"), Object ? Object->GetClass()->GetPathName() : TEXT(""));
	return Json;
}

void SetPageFields(const TSharedPtr<FJsonObject>& Json, int32 Total, int32 Offset, int32 Limit, int32 Count)
{
	Json->SetNumberField(TEXT("total"), Total);
	Json->SetNumberField(TEXT("offset"), Offset);
	Json->SetNumberField(TEXT("limit"), Limit);
	Json->SetNumberField(TEXT("count"), Count);
	Json->SetBoolField(TEXT("has_more"), static_cast<int64>(Offset) + Count < Total);
}

static FString ValueTypeToString(EInputActionValueType ValueType)
{
	switch (ValueType)
	{
	case EInputActionValueType::Boolean: return TEXT("Boolean");
	case EInputActionValueType::Axis1D:  return TEXT("Axis1D");
	case EInputActionValueType::Axis2D:  return TEXT("Axis2D");
	case EInputActionValueType::Axis3D:  return TEXT("Axis3D");
	default:                             return TEXT("Unknown");
	}
}

TSharedPtr<FJsonObject> InputActionToJson(const UInputAction* Action)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Action)
	{
		return Json;
	}

	Json->SetStringField(TEXT("asset_path"), Action->GetPathName());
	Json->SetStringField(TEXT("package_path"), Action->GetOutermost()->GetName());
	Json->SetStringField(TEXT("name"), Action->GetName());
	Json->SetStringField(TEXT("value_type"), ValueTypeToString(Action->ValueType));
	Json->SetStringField(TEXT("description"), Action->ActionDescription.ToString());
	Json->SetBoolField(TEXT("consume_input"), Action->bConsumeInput);
	Json->SetBoolField(TEXT("consume_legacy_mappings"), Action->bConsumesActionAndAxisMappings);
	Json->SetBoolField(TEXT("trigger_when_paused"), Action->bTriggerWhenPaused);
	Json->SetBoolField(TEXT("reserve_all_mappings"), Action->bReserveAllMappings);
	Json->SetStringField(TEXT("accumulation"),
		Action->AccumulationBehavior == EInputActionAccumulationBehavior::Cumulative
			? TEXT("Cumulative") : TEXT("TakeHighestAbsoluteValue"));

	bool bTriggersTruncated = false;
	bool bModifiersTruncated = false;
	Json->SetArrayField(TEXT("triggers"),
		DescribeInstancedArray(Action->Triggers, MaxInstancedPerAction, bTriggersTruncated));
	Json->SetArrayField(TEXT("modifiers"),
		DescribeInstancedArray(Action->Modifiers, MaxInstancedPerAction, bModifiersTruncated));
	Json->SetNumberField(TEXT("trigger_count"), Action->Triggers.Num());
	Json->SetNumberField(TEXT("modifier_count"), Action->Modifiers.Num());
	Json->SetBoolField(TEXT("triggers_truncated"), bTriggersTruncated);
	Json->SetBoolField(TEXT("modifiers_truncated"), bModifiersTruncated);
	Json->SetBoolField(TEXT("has_player_mappable_settings"), Action->GetPlayerMappableKeySettings() != nullptr);
	return Json;
}

TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("index"), Index);
	Json->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
	Json->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
	Json->SetStringField(TEXT("key"), Mapping.Key.ToString());
	Json->SetBoolField(TEXT("key_valid"), Mapping.Key.IsValid());
	Json->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());
	Json->SetStringField(TEXT("mapping_name"), Mapping.GetMappingName().ToString());
	Json->SetStringField(TEXT("display_name"), Mapping.GetDisplayName().ToString());
	Json->SetStringField(TEXT("display_category"), Mapping.GetDisplayCategory().ToString());

	bool bTriggersTruncated = false;
	bool bModifiersTruncated = false;
	Json->SetArrayField(TEXT("triggers"),
		DescribeInstancedArray(Mapping.Triggers, MaxInstancedPerMapping, bTriggersTruncated));
	Json->SetArrayField(TEXT("modifiers"),
		DescribeInstancedArray(Mapping.Modifiers, MaxInstancedPerMapping, bModifiersTruncated));
	Json->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());
	Json->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
	Json->SetBoolField(TEXT("triggers_truncated"), bTriggersTruncated);
	Json->SetBoolField(TEXT("modifiers_truncated"), bModifiersTruncated);
	return Json;
}

TSharedPtr<FJsonObject> MappingContextToJson(const UInputMappingContext* Context, int32 MappingOffset, int32 MappingLimit)
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	if (!Context)
	{
		return Json;
	}

	Json->SetStringField(TEXT("asset_path"), Context->GetPathName());
	Json->SetStringField(TEXT("package_path"), Context->GetOutermost()->GetName());
	Json->SetStringField(TEXT("name"), Context->GetName());
	Json->SetStringField(TEXT("description"), Context->ContextDescription.ToString());
	Json->SetBoolField(TEXT("filters_by_input_mode"), Context->ShouldFilterMappingByInputMode());
	Json->SetStringField(TEXT("registration_tracking_mode"),
		Context->GetRegistrationTrackingMode() == EMappingContextRegistrationTrackingMode::CountRegistrations
			? TEXT("CountRegistrations") : TEXT("Untracked"));

	// GetMappings() is the live DefaultKeyMappings.Mappings array. The legacy
	// UInputMappingContext::Mappings member is deprecated since UE 5.7 and is only
	// migrated in PostLoad for assets saved before the profile-overrides change, so
	// it is deliberately never read here.
	const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
	const int32 Start = FMath::Min(MappingOffset, Mappings.Num());
	const int32 End = static_cast<int32>(FMath::Min<int64>(
		Mappings.Num(), static_cast<int64>(MappingOffset) + MappingLimit));

	TArray<TSharedPtr<FJsonValue>> MappingValues;
	MappingValues.Reserve(FMath::Max(0, End - Start));
	for (int32 Index = Start; Index < End; ++Index)
	{
		MappingValues.Add(MakeShared<FJsonValueObject>(MappingToJson(Mappings[Index], Index)));
	}

	Json->SetArrayField(TEXT("mappings"), MappingValues);
	Json->SetNumberField(TEXT("mapping_count"), Mappings.Num());
	Json->SetNumberField(TEXT("mapping_offset"), MappingOffset);
	Json->SetNumberField(TEXT("mapping_limit"), MappingLimit);
	Json->SetNumberField(TEXT("mappings_returned"), MappingValues.Num());
	Json->SetBoolField(TEXT("mappings_truncated"), Start > 0 || End < Mappings.Num());
	Json->SetBoolField(TEXT("has_more_mappings"), End < Mappings.Num());
	return Json;
}

} // namespace MonolithInput
