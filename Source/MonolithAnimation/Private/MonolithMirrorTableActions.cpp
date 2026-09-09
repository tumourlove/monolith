#include "MonolithMirrorTableActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/UnrealType.h"
#include "Editor.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithMirrorTableActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// Task 2.1 — create_mirror_data_table
	Registry.RegisterAction(TEXT("animation"), TEXT("create_mirror_data_table"),
		TEXT("Create a UMirrorDataTable, populate find/replace expressions, and generate mirror rows for a skeleton"),
		FMonolithActionHandler::CreateStatic(&HandleCreateMirrorDataTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new mirror data table"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton to build the mirror table against"))
			.Optional(TEXT("find_replace"), TEXT("array"), TEXT("Array of {find,replace} pairs (default biped L/R rules)"))
			.Build());

	// Task 2.2 — set_schema_mirror_data_table
	Registry.RegisterAction(TEXT("animation"), TEXT("set_schema_mirror_data_table"),
		TEXT("Assign a mirror data table to a PoseSearch schema roled-skeleton slot"),
		FMonolithActionHandler::CreateStatic(&HandleSetSchemaMirrorDataTable),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("schema_path"), TEXT("PoseSearchSchema asset path"))
			.RequiredAssetPath(TEXT("mirror_table_path"), TEXT("UMirrorDataTable asset path"))
			.Optional(TEXT("skeleton_index"), TEXT("integer"), TEXT("Index into the schema's roled skeletons (default 0)"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Default biped Left/Right mirror rules (mirrors AnimationSettings.cpp defaults). */
static void PopulateDefaultBipedRules(TArray<FMirrorFindReplaceExpression>& Out)
{
	Out.Reset();
	Out.Add(FMirrorFindReplaceExpression(TEXT("r_"), TEXT("l_"), EMirrorFindReplaceMethod::Prefix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("l_"), TEXT("r_"), EMirrorFindReplaceMethod::Prefix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("_r"), TEXT("_l"), EMirrorFindReplaceMethod::Suffix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("_l"), TEXT("_r"), EMirrorFindReplaceMethod::Suffix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("_R"), TEXT("_L"), EMirrorFindReplaceMethod::Suffix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("_L"), TEXT("_R"), EMirrorFindReplaceMethod::Suffix));
	Out.Add(FMirrorFindReplaceExpression(TEXT("Right"), TEXT("Left"), EMirrorFindReplaceMethod::RegularExpression));
	Out.Add(FMirrorFindReplaceExpression(TEXT("Left"), TEXT("Right"), EMirrorFindReplaceMethod::RegularExpression));
}

/** Access the private 'Skeletons' UPROPERTY on UPoseSearchSchema via reflection (mirror of the
  * Channels reflection helper in MonolithPoseSearchActions.cpp). Returns nullptr on failure. */
static TArray<FPoseSearchRoledSkeleton>* GetSchemaSkeletonsMutable(UPoseSearchSchema* Schema)
{
	static FArrayProperty* CachedProp = nullptr;
	if (!CachedProp)
	{
		for (TFieldIterator<FArrayProperty> It(UPoseSearchSchema::StaticClass()); It; ++It)
		{
			if (It->GetName() == TEXT("Skeletons"))
			{
				CachedProp = *It;
				break;
			}
		}
	}
	if (!CachedProp) return nullptr;
	return CachedProp->ContainerPtrToValuePtr<TArray<FPoseSearchRoledSkeleton>>(Schema);
}

// ---------------------------------------------------------------------------
// Task 2.1 — create_mirror_data_table
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMirrorTableActions::HandleCreateMirrorDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UMirrorDataTable* MDT = NewObject<UMirrorDataTable>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!MDT) return FMonolithActionResult::Error(TEXT("Failed to create UMirrorDataTable object"));

	// FindReplaceMirroredNames() guards on RowStruct (MirrorDataTable.cpp:282) and adds zero rows
	// when it is unset. The engine factory uses FMirrorTableRow as the result struct.
	MDT->RowStruct = FMirrorTableRow::StaticStruct();

	MDT->Skeleton = Skeleton;

	// Populate find/replace expressions: explicit pairs if given, else default biped rules.
	const TArray<TSharedPtr<FJsonValue>>* FindReplaceArr = nullptr;
	if (Params->TryGetArrayField(TEXT("find_replace"), FindReplaceArr) && FindReplaceArr && FindReplaceArr->Num() > 0)
	{
		MDT->MirrorFindReplaceExpressions.Reset();
		for (const TSharedPtr<FJsonValue>& V : *FindReplaceArr)
		{
			const TSharedPtr<FJsonObject>* PairObj = nullptr;
			if (!V.IsValid() || !V->TryGetObject(PairObj) || !PairObj) continue;

			FString Find, Replace;
			(*PairObj)->TryGetStringField(TEXT("find"), Find);
			(*PairObj)->TryGetStringField(TEXT("replace"), Replace);
			if (Find.IsEmpty()) continue;

			// Optional per-pair method (Prefix/Suffix/RegularExpression); default Prefix.
			EMirrorFindReplaceMethod::Type Method = EMirrorFindReplaceMethod::Prefix;
			FString MethodStr;
			if ((*PairObj)->TryGetStringField(TEXT("method"), MethodStr))
			{
				if (MethodStr.Equals(TEXT("Suffix"), ESearchCase::IgnoreCase))
					Method = EMirrorFindReplaceMethod::Suffix;
				else if (MethodStr.Equals(TEXT("RegularExpression"), ESearchCase::IgnoreCase) || MethodStr.Equals(TEXT("Regex"), ESearchCase::IgnoreCase))
					Method = EMirrorFindReplaceMethod::RegularExpression;
			}

			MDT->MirrorFindReplaceExpressions.Add(
				FMirrorFindReplaceExpression(FName(*Find), FName(*Replace), Method));
		}
	}
	else
	{
		PopulateDefaultBipedRules(MDT->MirrorFindReplaceExpressions);
	}

	// Generate mirror rows from the find/replace rules against the skeleton's bone names.
	// UE 5.8 deprecates FindReplaceMirroredNames() in favour of
	// UpdateFromFindReplaceExpressions(), whose AddMissingOnly() preset is exactly what
	// the old entry point forwards to. UE 5.7 ships neither the new method nor
	// FFindReplaceOptions, so the call needs a version gate.
#if WITH_EDITOR
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	MDT->UpdateFromFindReplaceExpressions(UMirrorDataTable::FFindReplaceOptions::AddMissingOnly());
#else
	MDT->FindReplaceMirroredNames();
#endif
#endif

	FAssetRegistryModule::AssetCreated(MDT);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), MDT->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetNumberField(TEXT("expression_count"), MDT->MirrorFindReplaceExpressions.Num());
	Root->SetNumberField(TEXT("row_count"), MDT->GetRowMap().Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Task 2.2 — set_schema_mirror_data_table
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMirrorTableActions::HandleSetSchemaMirrorDataTable(const TSharedPtr<FJsonObject>& Params)
{
	FString SchemaPath = Params->GetStringField(TEXT("schema_path"));
	FString MirrorTablePath = Params->GetStringField(TEXT("mirror_table_path"));
	int32 SkeletonIndex = 0;
	if (Params->HasField(TEXT("skeleton_index")))
	{
		SkeletonIndex = static_cast<int32>(Params->GetNumberField(TEXT("skeleton_index")));
	}

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(SchemaPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *SchemaPath));

	UMirrorDataTable* MDT = FMonolithAssetUtils::LoadAssetByPath<UMirrorDataTable>(MirrorTablePath);
	if (!MDT)
		return FMonolithActionResult::Error(FString::Printf(TEXT("UMirrorDataTable not found: %s"), *MirrorTablePath));

	TArray<FPoseSearchRoledSkeleton>* Skeletons = GetSchemaSkeletonsMutable(Schema);
	if (!Skeletons)
		return FMonolithActionResult::Error(TEXT("Failed to access Skeletons property on schema via reflection"));

	if (SkeletonIndex < 0 || SkeletonIndex >= Skeletons->Num())
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid skeleton_index %d (schema has %d roled skeletons)"), SkeletonIndex, Skeletons->Num()));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Schema Mirror Data Table")));
	Schema->Modify();

	(*Skeletons)[SkeletonIndex].MirrorDataTable = MDT;

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_path"), SchemaPath);
	Root->SetNumberField(TEXT("skeleton_index"), SkeletonIndex);
	Root->SetStringField(TEXT("mirror_data_table"), MDT->GetPathName());
	return FMonolithActionResult::Success(Root);
}
