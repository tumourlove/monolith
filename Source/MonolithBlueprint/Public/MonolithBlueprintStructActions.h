#pragma once
#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithBlueprintStructActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult HandleCreateUserDefinedStruct(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCreateUserDefinedEnum(const TSharedPtr<FJsonObject>& Params);

	// Field-level editing of an EXISTING User Defined Struct. create_* can only
	// author a struct once; these make one editable afterwards.
	static FMonolithActionResult HandleGetStructFields(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddStructField(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRemoveStructField(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRenameStructField(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSetStructFieldType(const TSharedPtr<FJsonObject>& Params);

	// DataTable actions (Phase 3C)
	static FMonolithActionResult HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAddDataTableRow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetDataTableRows(const TSharedPtr<FJsonObject>& Params);

	// Raw UObject asset creation (not Blueprint)
	static FMonolithActionResult HandleCreateDataAsset(const TSharedPtr<FJsonObject>& Params);

	// Create + populate a DataAsset in one call (create_data_asset body + reflection-walker fill).
	static FMonolithActionResult HandleSeedDataAsset(const TSharedPtr<FJsonObject>& Params);
};
