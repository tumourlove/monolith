#pragma once
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

class FParamSchemaBuilder
{
public:
	// --- Required (no aliases) ---
	FParamSchemaBuilder& Required(const FString& Name, const FString& Type, const FString& Desc)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/true, /*Default=*/TEXT(""), /*bHasDefault=*/false, {});
		return *this;
	}

	// --- Required (with aliases) ---
	FParamSchemaBuilder& Required(const FString& Name, const FString& Type, const FString& Desc,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/true, /*Default=*/TEXT(""), /*bHasDefault=*/false, Aliases);
		return *this;
	}

	// --- Optional (with default, no aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		const FString& Default = TEXT(""))
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, Default, /*bHasDefault=*/!Default.IsEmpty(), {});
		return *this;
	}

	// --- Optional (with default + aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		const FString& Default, std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, Default, /*bHasDefault=*/!Default.IsEmpty(), Aliases);
		return *this;
	}

	// --- Optional (no default, with aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, /*Default=*/TEXT(""), /*bHasDefault=*/false, Aliases);
		return *this;
	}

	TSharedPtr<FJsonObject> Build()
	{
		return Schema;
	}

private:
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();

	void AddParam(const FString& Name, const FString& Type, const FString& Desc, bool bRequired,
		const FString& Default, bool bHasDefault, std::initializer_list<const TCHAR*> Aliases)
	{
		TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
		Param->SetStringField(TEXT("type"), Type);
		Param->SetStringField(TEXT("description"), Desc);
		Param->SetBoolField(TEXT("required"), bRequired);
		if (bHasDefault)
		{
			Param->SetStringField(TEXT("default"), Default);
		}
		if (Aliases.size() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> AliasArr;
			for (const TCHAR* A : Aliases)
			{
				AliasArr.Add(MakeShared<FJsonValueString>(FString(A)));
			}
			Param->SetArrayField(TEXT("aliases"), AliasArr);
		}
		Schema->SetObjectField(Name, Param);
	}
};

/**
 * Param-schema utilities for the tool registry.
 *
 * - ApplyAliases: rewrites alias keys in Params -> canonical schema keys before dispatch.
 *   Returns false if both alias and canonical are supplied (caller treats as ErrInvalidParams).
 * - FindUnknownKeys: returns Params keys that are neither canonical nor declared aliases.
 *   Used by K3 unknown-param warnings.
 * - IsStrictParamsEnabled: env-var STRICT_PARAMS=1 promotes K3 warnings to hard errors.
 */
class MONOLITHCORE_API FMonolithParamSchema
{
public:
	static bool ApplyAliases(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params, FString& OutCollision);
	static TArray<FString> FindUnknownKeys(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params);
	static bool IsStrictParamsEnabled();
};
