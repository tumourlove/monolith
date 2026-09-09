#include "MonolithPoseSearchActions.h"
#include "Runtime/Launch/Resources/Version.h"
#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchNormalizationSet.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchFeatureChannel_Velocity.h"
#include "PoseSearch/PoseSearchFeatureChannel_Heading.h"
#include "PoseSearch/PoseSearchFeatureChannel_Pose.h"
#include "PoseSearch/PoseSearchFeatureChannel_Trajectory.h"
#include "PoseSearch/PoseSearchFeatureChannel_Phase.h"
#include "PoseSearch/PoseSearchFeatureChannel_Distance.h"
#include "PoseSearch/PoseSearchFeatureChannel_Curve.h"
#include "PoseSearch/PoseSearchFeatureChannel_Group.h"
#include "PoseSearch/PoseSearchIndex.h"
#include "PoseSearch/PoseSearchDerivedData.h"
#include "PoseSearch/PoseSearchAnimNotifies.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "Animation/MirrorDataTable.h"
#include "AnimationBlueprintLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UnrealType.h"
#include "Editor.h"

// ---------------------------------------------------------------------------
// File-local static handlers (Motion Matching Pack — no header decl)
// ---------------------------------------------------------------------------

// Task 1.3 — set_database_normalization_set (file-local, no header edit)
static FMonolithActionResult HandleSetDatabaseNormalizationSet(const TSharedPtr<FJsonObject>& Params);
// Task 1.4 — add_database_entry (file-local, no header edit)
static FMonolithActionResult HandleAddDatabaseEntry(const TSharedPtr<FJsonObject>& Params);
// Task 2.3 — configure_schema_channel (file-local, no header edit)
static FMonolithActionResult HandleConfigureSchemaChannel(const TSharedPtr<FJsonObject>& Params);
// Task 2.4 — add_pose_search_notify (file-local, no header edit)
static FMonolithActionResult HandleAddPoseSearchNotify(const TSharedPtr<FJsonObject>& Params);
// Task 2.5 — derive_schema_channels_from_skeleton (file-local, no header edit)
static FMonolithActionResult HandleDeriveSchemaChannelsFromSkeleton(const TSharedPtr<FJsonObject>& Params);
// Task 2.6 — validate_pose_search_database (file-local, no header edit)
static FMonolithActionResult HandleValidatePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params);

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithPoseSearchActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("get_pose_search_schema"),
		TEXT("Get PoseSearch schema configuration including skeleton, sample rate, and channels"),
		FMonolithActionHandler::CreateStatic(&HandleGetPoseSearchSchema),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_pose_search_database"),
		TEXT("Get PoseSearch database contents including schema reference and animation asset list"),
		FMonolithActionHandler::CreateStatic(&HandleGetPoseSearchDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_database_sequence"),
		TEXT("Add an animation asset to a PoseSearch database"),
		FMonolithActionHandler::CreateStatic(&HandleAddDatabaseSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation asset to add"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable for search (default true)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_database_sequence"),
		TEXT("Remove an animation asset from a PoseSearch database by index"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveDatabaseSequence),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Required(TEXT("sequence_index"), TEXT("integer"), TEXT("Index of the animation asset to remove"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("get_database_stats"),
		TEXT("Get PoseSearch database statistics including sequence count, schema, and search index info"),
		FMonolithActionHandler::CreateStatic(&HandleGetDatabaseStats),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Build());

	// Wave 11 — PoseSearch Creation
	Registry.RegisterAction(TEXT("animation"), TEXT("create_pose_search_schema"),
		TEXT("Create a new PoseSearch schema with skeleton and default channels"),
		FMonolithActionHandler::CreateStatic(&HandleCreatePoseSearchSchema),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new schema (e.g. /Game/PoseSearch/PS_Schema)"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton asset path"))
			.Optional(TEXT("sample_rate"), TEXT("integer"), TEXT("Sample rate in Hz (default: 30)"))
			.Optional(TEXT("add_default_channels"), TEXT("boolean"), TEXT("Add default Pose+Trajectory channels (default: true)"))
			.Build());
	Registry.RegisterAction(TEXT("animation"), TEXT("create_pose_search_database"),
		TEXT("Create a new PoseSearch database and assign a schema"),
		FMonolithActionHandler::CreateStatic(&HandleCreatePoseSearchDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new database"))
			.RequiredAssetPath(TEXT("schema_path"), TEXT("PoseSearchSchema asset path to assign"))
			.Build());

	// Wave 14 — PoseSearch Writes
	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_sequence_properties"),
		TEXT("Set per-entry properties on a PoseSearch database animation asset (enabled, mirror, sampling range)"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseSequenceProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Required(TEXT("sequence_index"), TEXT("integer"), TEXT("Index of the animation entry"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable/disable for search"))
			.Optional(TEXT("mirror_option"), TEXT("string"), TEXT("UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"))
			.Optional(TEXT("sampling_range_start"), TEXT("number"), TEXT("Start of sampling range in seconds (0=full)"))
			.Optional(TEXT("sampling_range_end"), TEXT("number"), TEXT("End of sampling range in seconds (0=full)"))
			.Optional(TEXT("disable_reselection"), TEXT("boolean"), TEXT("Prevent reselection of poses from same asset"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_schema_channel"),
		TEXT("Add a feature channel to a PoseSearch schema (Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group)"),
		FMonolithActionHandler::CreateStatic(&HandleAddSchemaChannel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_type"), TEXT("string"), TEXT("Channel type: Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group"))
			.Optional(TEXT("weight"), TEXT("number"), TEXT("Channel weight (default 1.0)"))
			.Optional(TEXT("bone"), TEXT("string"), TEXT("Bone name for Position/Velocity/Heading channels"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("remove_schema_channel"),
		TEXT("Remove a feature channel from a PoseSearch schema by index (from authored channels)"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSchemaChannel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_index"), TEXT("integer"), TEXT("Index into the authored Channels array"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_channel_weight"),
		TEXT("Set the weight on a PoseSearch schema channel by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetChannelWeight),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_index"), TEXT("integer"), TEXT("Index into the authored Channels array"))
			.Required(TEXT("weight"), TEXT("number"), TEXT("New weight value"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("rebuild_pose_search_index"),
		TEXT("Trigger async rebuild of a PoseSearch database search index"),
		FMonolithActionHandler::CreateStatic(&HandleRebuildPoseSearchIndex),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Optional(TEXT("wait"), TEXT("boolean"), TEXT("Block until rebuild completes (default false)"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_search_mode"),
		TEXT("Configure search algorithm and cost bias settings on a PoseSearch database"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseSearchMode),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("PoseSearchDatabase asset path"))
			.Optional(TEXT("pose_search_mode"), TEXT("string"), TEXT("BruteForce, PCAKDTree, VPTree, or EventOnly"))
			.Optional(TEXT("kd_tree_query_num_neighbors"), TEXT("integer"), TEXT("Number of KNN neighbors (1-600, default 200)"))
			.Optional(TEXT("number_of_principal_components"), TEXT("integer"), TEXT("PCA dimensions (1-64, default 4)"))
			.Optional(TEXT("kd_tree_max_leaf_size"), TEXT("integer"), TEXT("KDTree max leaf size (1-256, default 16)"))
			.Optional(TEXT("continuing_pose_cost_bias"), TEXT("number"), TEXT("Cost bias for continuing current pose"))
			.Optional(TEXT("base_cost_bias"), TEXT("number"), TEXT("Base cost bias applied to all poses"))
			.Optional(TEXT("looping_cost_bias"), TEXT("number"), TEXT("Cost bias for looping animations"))
			.Optional(TEXT("continuing_interaction_cost_bias"), TEXT("number"), TEXT("Cost bias for continuing interaction poses"))
			.Build());

	// Motion Matching Pack — Sprint 1
	Registry.RegisterAction(TEXT("animation"), TEXT("create_normalization_set"),
		TEXT("Create a UPoseSearchNormalizationSet asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateNormalizationSet),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path for the new normalization set"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_database_to_normalization_set"),
		TEXT("Add a PoseSearch database to a normalization set"),
		FMonolithActionHandler::CreateStatic(&HandleAddDatabaseToNormalizationSet),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("set_path"), TEXT("PoseSearchNormalizationSet asset path"))
			.RequiredAssetPath(TEXT("database_path"), TEXT("PoseSearchDatabase asset path to add"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_normalization_set"),
		TEXT("Assign a normalization set to a PoseSearch database (editor-only)"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseNormalizationSet),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("database_path"), TEXT("PoseSearchDatabase asset path"))
			.RequiredAssetPath(TEXT("set_path"), TEXT("PoseSearchNormalizationSet asset path"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("add_database_entry"),
		TEXT("Add any UAnimationAsset (sequence/blendspace/composite) to a PoseSearch database with mirror options"),
		FMonolithActionHandler::CreateStatic(&HandleAddDatabaseEntry),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("database_path"), TEXT("PoseSearchDatabase asset path"))
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation asset to add (sequence, blendspace, or composite)"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable for search (default true)"))
			.Optional(TEXT("mirror_option"), TEXT("string"), TEXT("UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"))
			.Build());

	Registry.RegisterAction(TEXT("animation"), TEXT("set_database_entry_tags"),
		TEXT("Set mirror/reselection/enabled flags on a PoseSearch database entry by index"),
		FMonolithActionHandler::CreateStatic(&HandleSetDatabaseEntryTags),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("database_path"), TEXT("PoseSearchDatabase asset path"))
			.Required(TEXT("entry_index"), TEXT("integer"), TEXT("Index of the database animation entry"))
			.Optional(TEXT("mirror_option"), TEXT("string"), TEXT("UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"))
			.Optional(TEXT("disable_reselection"), TEXT("boolean"), TEXT("Prevent reselection of poses from same asset"))
			.Optional(TEXT("enabled"), TEXT("boolean"), TEXT("Enable/disable for search"))
			.Build());

	// Motion Matching Pack — Sprint 2

	// Task 2.3 — configure_schema_channel
	Registry.RegisterAction(TEXT("animation"), TEXT("configure_schema_channel"),
		TEXT("Configure a PoseSearch schema channel by index: trajectory samples (Trajectory) or sampled bones (Pose)"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureSchemaChannel),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("schema_path"), TEXT("PoseSearchSchema asset path"))
			.Required(TEXT("channel_index"), TEXT("integer"), TEXT("Index into the authored Channels array"))
			.Optional(TEXT("samples"), TEXT("array"), TEXT("Trajectory channel: array of {offset:number, flags:int, weight:number}"))
			.Optional(TEXT("sampled_bones"), TEXT("array"), TEXT("Pose channel: array of bone-name strings"))
			.Optional(TEXT("weight"), TEXT("number"), TEXT("Channel weight"))
			.Build());

	// Task 2.4 — add_pose_search_notify
	Registry.RegisterAction(TEXT("animation"), TEXT("add_pose_search_notify"),
		TEXT("Add a PoseSearch anim-notify-state (ExcludeFromDatabase, BlockTransition, ModifyCost, OverrideContinuingPoseCostBias, SamplingEvent, SamplingAttribute, BranchIn, IKWindow) to an animation"),
		FMonolithActionHandler::CreateStatic(&HandleAddPoseSearchNotify),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("anim_path"), TEXT("Animation sequence/composite asset path"))
			.Required(TEXT("notify_kind"), TEXT("string"), TEXT("ExcludeFromDatabase, BlockTransition, ModifyCost, OverrideContinuingPoseCostBias, SamplingEvent, SamplingAttribute, BranchIn, IKWindow"))
			.Required(TEXT("start_time"), TEXT("number"), TEXT("Notify start time in seconds"))
			.Required(TEXT("duration"), TEXT("number"), TEXT("Notify duration in seconds"))
			.Optional(TEXT("cost_bias"), TEXT("number"), TEXT("CostAddend for ModifyCost / OverrideContinuingPoseCostBias kinds"))
			.Build());

	// Task 2.5 — derive_schema_channels_from_skeleton
	Registry.RegisterAction(TEXT("animation"), TEXT("derive_schema_channels_from_skeleton"),
		TEXT("Heuristically append a Pose channel (foot bones) + a Trajectory channel (default samples) to a schema based on a skeleton's bone names"),
		FMonolithActionHandler::CreateStatic(&HandleDeriveSchemaChannelsFromSkeleton),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("schema_path"), TEXT("PoseSearchSchema asset path"))
			.RequiredAssetPath(TEXT("skeleton_path"), TEXT("Skeleton to derive foot bones from"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Channel preset (default \"biped\")"))
			.Build());

	// Task 2.6 — validate_pose_search_database
	Registry.RegisterAction(TEXT("animation"), TEXT("validate_pose_search_database"),
		TEXT("Validate a PoseSearch database: schema present, per-entry skeleton compatibility, and search-index freshness (no mutation)"),
		FMonolithActionHandler::CreateStatic(&HandleValidatePoseSearchDatabase),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("database_path"), TEXT("PoseSearchDatabase asset path"))
			.Build());
}

// ---------------------------------------------------------------------------
// get_pose_search_schema
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetPoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);

	// Skeletons
	const TArray<FPoseSearchRoledSkeleton>& RoledSkeletons = Schema->GetRoledSkeletons();
	if (RoledSkeletons.Num() > 0)
	{
		const FPoseSearchRoledSkeleton& DefaultSkeleton = RoledSkeletons[0];
		if (DefaultSkeleton.Skeleton)
		{
			Root->SetStringField(TEXT("skeleton"), DefaultSkeleton.Skeleton->GetPathName());
		}

		// All roled skeletons
		TArray<TSharedPtr<FJsonValue>> SkeletonArray;
		for (int32 i = 0; i < RoledSkeletons.Num(); ++i)
		{
			TSharedPtr<FJsonObject> SkelObj = MakeShared<FJsonObject>();
			SkelObj->SetNumberField(TEXT("index"), i);
			SkelObj->SetStringField(TEXT("role"), RoledSkeletons[i].Role.ToString());
			if (RoledSkeletons[i].Skeleton)
			{
				SkelObj->SetStringField(TEXT("skeleton"), RoledSkeletons[i].Skeleton->GetPathName());
			}
			// §12b R2: per-skeleton mirror_data_table ref (path or null) — needed by task 2.2 read-back.
			if (RoledSkeletons[i].MirrorDataTable)
			{
				SkelObj->SetStringField(TEXT("mirror_data_table"), RoledSkeletons[i].MirrorDataTable->GetPathName());
			}
			else
			{
				SkelObj->SetField(TEXT("mirror_data_table"), MakeShared<FJsonValueNull>());
			}
			SkeletonArray.Add(MakeShared<FJsonValueObject>(SkelObj));
		}
		Root->SetArrayField(TEXT("skeletons"), SkeletonArray);
	}

	// Channels
	TConstArrayView<TObjectPtr<UPoseSearchFeatureChannel>> Channels = Schema->GetChannels();
	TArray<TSharedPtr<FJsonValue>> ChannelArray;
	for (int32 i = 0; i < Channels.Num(); ++i)
	{
		const UPoseSearchFeatureChannel* Channel = Channels[i];
		if (!Channel) continue;

		TSharedPtr<FJsonObject> ChObj = MakeShared<FJsonObject>();
		ChObj->SetNumberField(TEXT("index"), i);
		ChObj->SetStringField(TEXT("type"), Channel->GetClass()->GetName());
		ChObj->SetNumberField(TEXT("cardinality"), Channel->GetChannelCardinality());
		ChObj->SetNumberField(TEXT("data_offset"), Channel->GetChannelDataOffset());

		// §12b R3: per-channel sample/bone detail — needed by tasks 2.3, 2.5 read-backs.
		if (const UPoseSearchFeatureChannel_Trajectory* TrajChannel = Cast<UPoseSearchFeatureChannel_Trajectory>(Channel))
		{
			ChObj->SetNumberField(TEXT("sample_count"), TrajChannel->Samples.Num());
			TArray<TSharedPtr<FJsonValue>> SampleArr;
			for (const FPoseSearchTrajectorySample& S : TrajChannel->Samples)
			{
				TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
				SObj->SetNumberField(TEXT("offset"), S.Offset);
				SObj->SetNumberField(TEXT("flags"), S.Flags);
#if WITH_EDITORONLY_DATA
				SObj->SetNumberField(TEXT("weight"), S.Weight);
#endif
				SampleArr.Add(MakeShared<FJsonValueObject>(SObj));
			}
			ChObj->SetArrayField(TEXT("samples"), SampleArr);
		}
		else if (const UPoseSearchFeatureChannel_Pose* PoseChannel = Cast<UPoseSearchFeatureChannel_Pose>(Channel))
		{
			ChObj->SetNumberField(TEXT("bone_count"), PoseChannel->SampledBones.Num());
			TArray<TSharedPtr<FJsonValue>> BoneArr;
			for (const FPoseSearchBone& B : PoseChannel->SampledBones)
			{
				BoneArr.Add(MakeShared<FJsonValueString>(B.Reference.BoneName.ToString()));
			}
			ChObj->SetArrayField(TEXT("bones"), BoneArr);
		}

		ChannelArray.Add(MakeShared<FJsonValueObject>(ChObj));
	}
	Root->SetArrayField(TEXT("channels"), ChannelArray);
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// get_pose_search_database
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetPoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	// Schema reference
	if (Database->Schema)
	{
		Root->SetStringField(TEXT("schema"), Database->Schema->GetPathName());
	}

	// Animation assets
	const int32 NumAssets = Database->GetNumAnimationAssets();
	Root->SetNumberField(TEXT("sequence_count"), NumAssets);
	// §12b R1: explicit entry count alias for MM-pack read-backs
	Root->SetNumberField(TEXT("count"), NumAssets);

	TArray<TSharedPtr<FJsonValue>> SeqArray;
	for (int32 i = 0; i < NumAssets; ++i)
	{
		const FPoseSearchDatabaseAnimationAsset* AnimAsset = Database->GetDatabaseAnimationAsset(i);
		if (!AnimAsset) continue;

		TSharedPtr<FJsonObject> SeqObj = MakeShared<FJsonObject>();
		SeqObj->SetNumberField(TEXT("index"), i);

		if (UObject* Asset = AnimAsset->GetAnimationAsset())
		{
			SeqObj->SetStringField(TEXT("animation"), Asset->GetPathName());
			SeqObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
		}

#if WITH_EDITORONLY_DATA
		SeqObj->SetBoolField(TEXT("enabled"), AnimAsset->IsEnabled());
		SeqObj->SetBoolField(TEXT("disable_reselection"), AnimAsset->IsDisableReselection());
		FFloatInterval Range = AnimAsset->GetSamplingRange();
		SeqObj->SetNumberField(TEXT("sampling_range_start"), Range.Min);
		SeqObj->SetNumberField(TEXT("sampling_range_end"), Range.Max);
		SeqObj->SetStringField(TEXT("mirror_option"),
			AnimAsset->GetMirrorOption() == EPoseSearchMirrorOption::UnmirroredOnly ? TEXT("UnmirroredOnly") :
			AnimAsset->GetMirrorOption() == EPoseSearchMirrorOption::MirroredOnly ? TEXT("MirroredOnly") :
			TEXT("UnmirroredAndMirrored"));
#endif

		SeqArray.Add(MakeShared<FJsonValueObject>(SeqObj));
	}
	Root->SetArrayField(TEXT("sequences"), SeqArray);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_database_sequence
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleAddDatabaseSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));
	bool bEnabled = true;
	if (Params->HasField(TEXT("enabled")))
	{
		bEnabled = Params->GetBoolField(TEXT("enabled"));
	}

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	UObject* AnimAsset = FMonolithAssetUtils::LoadAssetByPath<UObject>(AnimPath);
	if (!AnimAsset)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

	// Check if already in database
	if (Database->Contains(AnimAsset))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation already in database: %s"), *AnimPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Database Animation")));
	Database->Modify();

	FPoseSearchDatabaseAnimationAsset NewEntry;
	NewEntry.AnimAsset = AnimAsset;
#if WITH_EDITORONLY_DATA
	NewEntry.bEnabled = bEnabled;
#endif

	const int32 IndexBefore = Database->GetNumAnimationAssets();
	Database->AddAnimationAsset(NewEntry);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), IndexBefore);
	Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	Root->SetBoolField(TEXT("enabled"), bEnabled);
	Root->SetNumberField(TEXT("new_count"), Database->GetNumAnimationAssets());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_database_sequence
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRemoveDatabaseSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SequenceIndex = static_cast<int32>(Params->GetNumberField(TEXT("sequence_index")));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	const int32 NumAssets = Database->GetNumAnimationAssets();
	if (SequenceIndex < 0 || SequenceIndex >= NumAssets)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sequence index: %d (database has %d entries)"), SequenceIndex, NumAssets));

	// Capture info before removal
	FString RemovedAnimPath;
	if (const FPoseSearchDatabaseAnimationAsset* AnimAsset = Database->GetDatabaseAnimationAsset(SequenceIndex))
	{
		if (UObject* Asset = AnimAsset->GetAnimationAsset())
		{
			RemovedAnimPath = Asset->GetPathName();
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove PoseSearch Database Animation")));
	Database->Modify();

	Database->RemoveAnimationAssetAt(SequenceIndex);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("removed_index"), SequenceIndex);
	Root->SetStringField(TEXT("removed_animation"), RemovedAnimPath);
	Root->SetNumberField(TEXT("remaining_count"), Database->GetNumAnimationAssets());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// get_database_stats
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleGetDatabaseStats(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	const int32 EntryCount = Database->GetNumAnimationAssets();
	Root->SetNumberField(TEXT("sequence_count"), EntryCount);
	// §12b R1: explicit entry count (alias of sequence_count for MM-pack read-backs)
	Root->SetNumberField(TEXT("count"), EntryCount);

	// §12b R1: normalization_set ref (path or null) — needed by task 1.3 read-back
#if WITH_EDITORONLY_DATA
	if (Database->NormalizationSet)
	{
		Root->SetStringField(TEXT("normalization_set"), Database->NormalizationSet->GetPathName());
	}
	else
	{
		Root->SetField(TEXT("normalization_set"), MakeShared<FJsonValueNull>());
	}
#else
	Root->SetField(TEXT("normalization_set"), MakeShared<FJsonValueNull>());
#endif

	// §12b R1: per-entry flags — needed by tasks 1.2, 1.4, 1.5 read-backs
	{
		TArray<TSharedPtr<FJsonValue>> EntryArray;
		for (int32 i = 0; i < EntryCount; ++i)
		{
			const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(i);
			if (!Entry) continue;

			TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
			EntryObj->SetNumberField(TEXT("index"), i);
			if (UObject* Asset = Entry->GetAnimationAsset())
			{
				EntryObj->SetStringField(TEXT("animation"), Asset->GetPathName());
				EntryObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
			}
#if WITH_EDITORONLY_DATA
			EntryObj->SetBoolField(TEXT("enabled"), Entry->IsEnabled());
			EntryObj->SetBoolField(TEXT("disable_reselection"), Entry->IsDisableReselection());
			EntryObj->SetStringField(TEXT("mirror_option"),
				Entry->GetMirrorOption() == EPoseSearchMirrorOption::UnmirroredOnly ? TEXT("UnmirroredOnly") :
				Entry->GetMirrorOption() == EPoseSearchMirrorOption::MirroredOnly ? TEXT("MirroredOnly") :
				TEXT("UnmirroredAndMirrored"));
#endif
			EntryArray.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Root->SetArrayField(TEXT("entries"), EntryArray);
	}

	// Schema
	if (Database->Schema)
	{
		Root->SetStringField(TEXT("schema"), Database->Schema->GetPathName());
		Root->SetNumberField(TEXT("sample_rate"), Database->Schema->SampleRate);
		Root->SetNumberField(TEXT("schema_cardinality"), Database->Schema->SchemaCardinality);
	}

	// Search index stats — GUARD: UPoseSearchDatabase::GetSearchIndex() check()-asserts
	// (PoseSearchDatabase.cpp:1135) when the database has never been indexed. Mirror the
	// engine's own gate (GetSkipSearchIfPossible, PoseSearchDatabase.cpp:1561): only touch
	// the search index once an editor-time build has completed successfully.
	{
#if WITH_EDITOR
		using namespace UE::PoseSearch;
		const bool bIndexBuilt = Database->Schema &&
			EAsyncBuildIndexResult::Success ==
			FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(
				Database, ERequestAsyncBuildFlag::ContinueRequest | ERequestAsyncBuildFlag::WaitForCompletion);
#else
		const bool bIndexBuilt = false;
#endif
		Root->SetBoolField(TEXT("index_built"), bIndexBuilt);
		if (bIndexBuilt)
		{
			const int32 NumPoses = Database->GetSearchIndex().GetNumPoses();
			Root->SetNumberField(TEXT("total_pose_count"), NumPoses);
			Root->SetBoolField(TEXT("is_valid"), NumPoses > 0);
		}
		else
		{
			Root->SetField(TEXT("total_pose_count"), MakeShared<FJsonValueNull>());
			Root->SetBoolField(TEXT("is_valid"), false);
		}
	}

	// Search mode
	FString SearchModeStr;
	switch (Database->PoseSearchMode)
	{
	case EPoseSearchMode::BruteForce: SearchModeStr = TEXT("BruteForce"); break;
	case EPoseSearchMode::PCAKDTree: SearchModeStr = TEXT("PCAKDTree"); break;
	case EPoseSearchMode::VPTree: SearchModeStr = TEXT("VPTree"); break;
	case EPoseSearchMode::EventOnly: SearchModeStr = TEXT("EventOnly"); break;
	default: SearchModeStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("search_mode"), SearchModeStr);

	// Cost biases
	Root->SetNumberField(TEXT("continuing_pose_cost_bias"), Database->ContinuingPoseCostBias);
	Root->SetNumberField(TEXT("base_cost_bias"), Database->BaseCostBias);
	Root->SetNumberField(TEXT("looping_cost_bias"), Database->LoopingCostBias);

	// Tags
	if (Database->Tags.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> TagArray;
		for (const FName& Tag : Database->Tags)
		{
			TagArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Root->SetArrayField(TEXT("tags"), TagArray);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_pose_search_schema
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleCreatePoseSearchSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton) return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UPoseSearchSchema* Schema = NewObject<UPoseSearchSchema>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Schema) return FMonolithActionResult::Error(TEXT("Failed to create UPoseSearchSchema object"));

	// Add skeleton (public API)
	Schema->AddSkeleton(Skeleton);

	// Set sample rate if provided
	if (Params->HasField(TEXT("sample_rate")))
	{
		Schema->SampleRate = static_cast<int32>(Params->GetNumberField(TEXT("sample_rate")));
	}

	// Add default channels (Pose + Trajectory) unless explicitly disabled
	bool bAddDefaults = true;
	if (Params->HasField(TEXT("add_default_channels")))
	{
		bAddDefaults = Params->GetBoolField(TEXT("add_default_channels"));
	}
	if (bAddDefaults)
	{
		Schema->AddDefaultChannels();
	}

	FAssetRegistryModule::AssetCreated(Schema);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Schema->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);
	Root->SetBoolField(TEXT("default_channels_added"), bAddDefaults);

	// Report channel count
	TConstArrayView<TObjectPtr<UPoseSearchFeatureChannel>> Channels = Schema->GetChannels();
	Root->SetNumberField(TEXT("channel_count"), Channels.Num());

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// create_pose_search_database
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleCreatePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString SchemaPath = Params->GetStringField(TEXT("schema_path"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(SchemaPath);
	if (!Schema) return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *SchemaPath));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UPoseSearchDatabase* Database = NewObject<UPoseSearchDatabase>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Database) return FMonolithActionResult::Error(TEXT("Failed to create UPoseSearchDatabase object"));

	Database->Schema = Schema;

	FAssetRegistryModule::AssetCreated(Database);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Database->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	Root->SetStringField(TEXT("schema"), Schema->GetPathName());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Helpers — Schema channel reflection access
// ---------------------------------------------------------------------------

/** Access the private 'Channels' UPROPERTY on UPoseSearchSchema via reflection.
  * Returns nullptr if the property is not found (should never happen). */
static FArrayProperty* GetSchemaChannelsProperty()
{
	static FArrayProperty* CachedProp = nullptr;
	if (!CachedProp)
	{
		for (TFieldIterator<FArrayProperty> It(UPoseSearchSchema::StaticClass()); It; ++It)
		{
			if (It->GetName() == TEXT("Channels"))
			{
				CachedProp = *It;
				break;
			}
		}
	}
	return CachedProp;
}

/** Get a mutable pointer to the authored Channels TArray on a schema instance. */
static TArray<TObjectPtr<UPoseSearchFeatureChannel>>* GetSchemaChannelsMutable(UPoseSearchSchema* Schema)
{
	FArrayProperty* ArrayProp = GetSchemaChannelsProperty();
	if (!ArrayProp) return nullptr;
	return ArrayProp->ContainerPtrToValuePtr<TArray<TObjectPtr<UPoseSearchFeatureChannel>>>(Schema);
}

/** Trigger PostEditChangeProperty on the schema to call Finalize() internally. */
static void TriggerSchemaFinalize(UPoseSearchSchema* Schema)
{
#if WITH_EDITOR
	FPropertyChangedEvent Evt(GetSchemaChannelsProperty(), EPropertyChangeType::ValueSet);
	Schema->PostEditChangeProperty(Evt);
#endif
}

/** Map a channel type string to a UClass. Returns nullptr if unrecognized. */
static UClass* ResolveChannelClass(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("Position"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Position::StaticClass();
	if (TypeStr.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Velocity::StaticClass();
	if (TypeStr.Equals(TEXT("Heading"), ESearchCase::IgnoreCase))  return UPoseSearchFeatureChannel_Heading::StaticClass();
	if (TypeStr.Equals(TEXT("Pose"), ESearchCase::IgnoreCase))     return UPoseSearchFeatureChannel_Pose::StaticClass();
	if (TypeStr.Equals(TEXT("Trajectory"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Trajectory::StaticClass();
	if (TypeStr.Equals(TEXT("Phase"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Phase::StaticClass();
	if (TypeStr.Equals(TEXT("Distance"), ESearchCase::IgnoreCase)) return UPoseSearchFeatureChannel_Distance::StaticClass();
	if (TypeStr.Equals(TEXT("Curve"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Curve::StaticClass();
	if (TypeStr.Equals(TEXT("Group"), ESearchCase::IgnoreCase))    return UPoseSearchFeatureChannel_Group::StaticClass();
	return nullptr;
}

// ---------------------------------------------------------------------------
// Database animation-asset write-back (engine-version compat)
//
// UE 5.8 deprecated UPoseSearchDatabase::GetMutableDatabaseAnimationAsset in
// favour of the SetAnimationAssetAt setter, which UE 5.7 does not ship. Handlers
// therefore read an entry through the const accessor (present on both engines),
// mutate the copy, and commit it here. Entries live in a plain
// TArray<FPoseSearchDatabaseAnimationAsset>, so copying does not slice.
// ---------------------------------------------------------------------------

static void MonolithCommitDatabaseAnimationAsset(
	UPoseSearchDatabase* Database,
	const FPoseSearchDatabaseAnimationAsset& Entry,
	int32 AnimationAssetIndex)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	Database->SetAnimationAssetAt(Entry, AnimationAssetIndex);
#else
	if (FPoseSearchDatabaseAnimationAsset* Dest = Database->GetMutableDatabaseAnimationAsset(AnimationAssetIndex))
	{
		*Dest = Entry;
	}
#endif
}

// ---------------------------------------------------------------------------
// set_database_sequence_properties — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetDatabaseSequenceProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 SeqIndex = static_cast<int32>(Params->GetNumberField(TEXT("sequence_index")));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	const int32 NumAssets = Database->GetNumAnimationAssets();
	if (SeqIndex < 0 || SeqIndex >= NumAssets)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid sequence_index %d (database has %d entries)"), SeqIndex, NumAssets));

	const FPoseSearchDatabaseAnimationAsset* SourceEntry = Database->GetDatabaseAnimationAsset(SeqIndex);
	if (!SourceEntry)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get entry at index %d"), SeqIndex));

	// Mutate a copy; MonolithCommitDatabaseAnimationAsset writes it back before every exit.
	FPoseSearchDatabaseAnimationAsset EntryCopy = *SourceEntry;
	FPoseSearchDatabaseAnimationAsset* Entry = &EntryCopy;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Database Sequence Properties")));
	Database->Modify();

#if WITH_EDITORONLY_DATA
	if (Params->HasField(TEXT("enabled")))
	{
		Entry->SetIsEnabled(Params->GetBoolField(TEXT("enabled")));
	}

	if (Params->HasField(TEXT("disable_reselection")))
	{
		Entry->SetDisableReselection(Params->GetBoolField(TEXT("disable_reselection")));
	}

	if (Params->HasField(TEXT("mirror_option")))
	{
		FString MirrorStr = Params->GetStringField(TEXT("mirror_option"));
		if (MirrorStr.Equals(TEXT("UnmirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredOnly;
		else if (MirrorStr.Equals(TEXT("MirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::MirroredOnly;
		else if (MirrorStr.Equals(TEXT("UnmirroredAndMirrored"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredAndMirrored;
		else
		{
			MonolithCommitDatabaseAnimationAsset(Database, EntryCopy, SeqIndex);
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mirror_option: '%s'. Use UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"), *MirrorStr));
		}
	}

	if (Params->HasField(TEXT("sampling_range_start")) || Params->HasField(TEXT("sampling_range_end")))
	{
		FFloatInterval CurrentRange = Entry->GetSamplingRange();
		float Start = Params->HasField(TEXT("sampling_range_start"))
			? static_cast<float>(Params->GetNumberField(TEXT("sampling_range_start")))
			: CurrentRange.Min;
		float End = Params->HasField(TEXT("sampling_range_end"))
			? static_cast<float>(Params->GetNumberField(TEXT("sampling_range_end")))
			: CurrentRange.Max;
		Entry->SetSamplingRange(FFloatInterval(Start, End));
	}
#endif // WITH_EDITORONLY_DATA

	MonolithCommitDatabaseAnimationAsset(Database, EntryCopy, SeqIndex);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	// Build response — read back current state
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("sequence_index"), SeqIndex);

	if (UObject* AnimAsset = Entry->GetAnimationAsset())
	{
		Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	}

#if WITH_EDITORONLY_DATA
	Root->SetBoolField(TEXT("enabled"), Entry->IsEnabled());
	Root->SetBoolField(TEXT("disable_reselection"), Entry->IsDisableReselection());

	FString MirrorStr;
	switch (Entry->GetMirrorOption())
	{
	case EPoseSearchMirrorOption::UnmirroredOnly:      MirrorStr = TEXT("UnmirroredOnly"); break;
	case EPoseSearchMirrorOption::MirroredOnly:        MirrorStr = TEXT("MirroredOnly"); break;
	case EPoseSearchMirrorOption::UnmirroredAndMirrored: MirrorStr = TEXT("UnmirroredAndMirrored"); break;
	default:                                           MirrorStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("mirror_option"), MirrorStr);

	FFloatInterval Range = Entry->GetSamplingRange();
	Root->SetNumberField(TEXT("sampling_range_start"), Range.Min);
	Root->SetNumberField(TEXT("sampling_range_end"), Range.Max);
#endif

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// add_schema_channel — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleAddSchemaChannel(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ChannelType = Params->GetStringField(TEXT("channel_type"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	UClass* ChannelClass = ResolveChannelClass(ChannelType);
	if (!ChannelClass)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown channel type: '%s'. Supported: Position, Velocity, Heading, Pose, Trajectory, Phase, Distance, Curve, Group"), *ChannelType));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Schema Channel")));
	Schema->Modify();

	// Create the channel outered to the schema
	UPoseSearchFeatureChannel* Channel = NewObject<UPoseSearchFeatureChannel>(Schema, ChannelClass, NAME_None, RF_Transactional);
	if (!Channel)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create channel of type %s"), *ChannelType));
	}

	// Set weight if provided (Weight is WITH_EDITORONLY_DATA, exists on all channel types as a float UPROPERTY)
#if WITH_EDITORONLY_DATA
	if (Params->HasField(TEXT("weight")))
	{
		float Weight = static_cast<float>(Params->GetNumberField(TEXT("weight")));
		FProperty* WeightProp = Channel->GetClass()->FindPropertyByName(TEXT("Weight"));
		if (WeightProp)
		{
			void* WeightPtr = WeightProp->ContainerPtrToValuePtr<void>(Channel);
			FString WeightStr = FString::SanitizeFloat(Weight);
			WeightProp->ImportText_Direct(*WeightStr, WeightPtr, Channel, PPF_None);
		}
	}

	// Set bone reference if provided (for Position, Velocity, Heading channels)
	if (Params->HasField(TEXT("bone")))
	{
		FString BoneName = Params->GetStringField(TEXT("bone"));
		FProperty* BoneProp = Channel->GetClass()->FindPropertyByName(TEXT("Bone"));
		if (BoneProp)
		{
			// FBoneReference is a struct with BoneName field — import using UE text format
			void* BonePtr = BoneProp->ContainerPtrToValuePtr<void>(Channel);
			FString BoneImportStr = FString::Printf(TEXT("(BoneName=\"%s\")"), *BoneName);
			BoneProp->ImportText_Direct(*BoneImportStr, BonePtr, Channel, PPF_None);
		}
	}
#endif // WITH_EDITORONLY_DATA

	// Use the public AddChannel API
	Schema->AddChannel(Channel);

	// Trigger Finalize() via PostEditChangeProperty
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	// Count authored channels for response
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* AuthoredChannels = GetSchemaChannelsMutable(Schema);
	int32 AuthoredCount = AuthoredChannels ? AuthoredChannels->Num() : -1;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("channel_type"), ChannelType);
	Root->SetStringField(TEXT("channel_class"), Channel->GetClass()->GetName());
	Root->SetNumberField(TEXT("authored_channel_count"), AuthoredCount);
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_schema_channel — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRemoveSchemaChannel(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 ChannelIndex = static_cast<int32>(Params->GetNumberField(TEXT("channel_index")));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	// Access the private Channels array via reflection
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetSchemaChannelsMutable(Schema);
	if (!Channels)
		return FMonolithActionResult::Error(TEXT("Failed to access Channels property on schema via reflection"));

	if (ChannelIndex < 0 || ChannelIndex >= Channels->Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid channel_index %d (schema has %d authored channels)"), ChannelIndex, Channels->Num()));

	// Capture info before removal
	FString RemovedClass = (*Channels)[ChannelIndex] ? (*Channels)[ChannelIndex]->GetClass()->GetName() : TEXT("null");

	GEditor->BeginTransaction(FText::FromString(TEXT("Remove PoseSearch Schema Channel")));
	Schema->Modify();

	Channels->RemoveAt(ChannelIndex);

	// Trigger Finalize() via PostEditChangeProperty
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("removed_index"), ChannelIndex);
	Root->SetStringField(TEXT("removed_class"), RemovedClass);
	Root->SetNumberField(TEXT("remaining_channel_count"), Channels->Num());
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_channel_weight — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetChannelWeight(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 ChannelIndex = static_cast<int32>(Params->GetNumberField(TEXT("channel_index")));
	float NewWeight = static_cast<float>(Params->GetNumberField(TEXT("weight")));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(AssetPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *AssetPath));

	// Access authored channels via reflection
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetSchemaChannelsMutable(Schema);
	if (!Channels)
		return FMonolithActionResult::Error(TEXT("Failed to access Channels property on schema via reflection"));

	if (ChannelIndex < 0 || ChannelIndex >= Channels->Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid channel_index %d (schema has %d authored channels)"), ChannelIndex, Channels->Num()));

	UPoseSearchFeatureChannel* Channel = (*Channels)[ChannelIndex];
	if (!Channel)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Channel at index %d is null"), ChannelIndex));

	// Weight is WITH_EDITORONLY_DATA — use reflection to set it on any channel type
#if WITH_EDITORONLY_DATA
	FProperty* WeightProp = Channel->GetClass()->FindPropertyByName(TEXT("Weight"));
	if (!WeightProp)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Channel class %s has no Weight property"), *Channel->GetClass()->GetName()));

	void* WeightPtr = WeightProp->ContainerPtrToValuePtr<void>(Channel);

	// Read old weight
	FString OldWeightStr;
	WeightProp->ExportText_Direct(OldWeightStr, WeightPtr, WeightPtr, Channel, PPF_None);

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Channel Weight")));
	Schema->Modify();

	FString WeightStr = FString::SanitizeFloat(NewWeight);
	WeightProp->ImportText_Direct(*WeightStr, WeightPtr, Channel, PPF_None);

	// Trigger Finalize()
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetNumberField(TEXT("channel_index"), ChannelIndex);
	Root->SetStringField(TEXT("channel_class"), Channel->GetClass()->GetName());
	Root->SetStringField(TEXT("old_weight"), OldWeightStr);
	Root->SetNumberField(TEXT("new_weight"), NewWeight);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("Channel weights are only available in editor builds (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// rebuild_pose_search_index — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleRebuildPoseSearchIndex(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	bool bWait = false;
	if (Params->HasField(TEXT("wait")))
	{
		bWait = Params->GetBoolField(TEXT("wait"));
	}

	using namespace UE::PoseSearch;
	ERequestAsyncBuildFlag Flag = ERequestAsyncBuildFlag::NewRequest;
	if (bWait)
	{
		Flag |= ERequestAsyncBuildFlag::WaitForCompletion;
	}

	EAsyncBuildIndexResult Result = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, Flag);

	FString ResultStr;
	switch (Result)
	{
	case EAsyncBuildIndexResult::InProgress: ResultStr = TEXT("InProgress"); break;
	case EAsyncBuildIndexResult::Success:    ResultStr = TEXT("Success"); break;
	case EAsyncBuildIndexResult::Failed:     ResultStr = TEXT("Failed"); break;
	default:                                 ResultStr = TEXT("Unknown"); break;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);
	Root->SetStringField(TEXT("result"), ResultStr);
	Root->SetBoolField(TEXT("waited"), bWait);

	// Report current index stats
	const FSearchIndex& SearchIndex = Database->GetSearchIndex();
	Root->SetNumberField(TEXT("total_poses"), SearchIndex.GetNumPoses());

	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("rebuild_pose_search_index is only available in editor builds"));
#endif
}

// ---------------------------------------------------------------------------
// set_database_search_mode — Wave 14
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetDatabaseSearchMode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(AssetPath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *AssetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Database Search Mode")));
	Database->Modify();

	// Search mode
	if (Params->HasField(TEXT("pose_search_mode")))
	{
		FString ModeStr = Params->GetStringField(TEXT("pose_search_mode"));
		if (ModeStr.Equals(TEXT("BruteForce"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::BruteForce;
		else if (ModeStr.Equals(TEXT("PCAKDTree"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::PCAKDTree;
		else if (ModeStr.Equals(TEXT("VPTree"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::VPTree;
		else if (ModeStr.Equals(TEXT("EventOnly"), ESearchCase::IgnoreCase))
			Database->PoseSearchMode = EPoseSearchMode::EventOnly;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid pose_search_mode: '%s'. Use BruteForce, PCAKDTree, VPTree, or EventOnly"), *ModeStr));
		}
	}

	// KDTree neighbors (not editor-only)
	if (Params->HasField(TEXT("kd_tree_query_num_neighbors")))
	{
		Database->KDTreeQueryNumNeighbors = FMath::Clamp(
			static_cast<int32>(Params->GetNumberField(TEXT("kd_tree_query_num_neighbors"))), 1, 600);
	}

#if WITH_EDITORONLY_DATA
	// PCA components (editor-only)
	if (Params->HasField(TEXT("number_of_principal_components")))
	{
		Database->NumberOfPrincipalComponents = FMath::Clamp(
			static_cast<int32>(Params->GetNumberField(TEXT("number_of_principal_components"))), 1, 64);
	}

	// KDTree max leaf size (editor-only)
	if (Params->HasField(TEXT("kd_tree_max_leaf_size")))
	{
		Database->KDTreeMaxLeafSize = FMath::Clamp(
			static_cast<int32>(Params->GetNumberField(TEXT("kd_tree_max_leaf_size"))), 1, 256);
	}
#endif // WITH_EDITORONLY_DATA

	// Cost biases (runtime-accessible)
	if (Params->HasField(TEXT("continuing_pose_cost_bias")))
	{
		Database->ContinuingPoseCostBias = static_cast<float>(Params->GetNumberField(TEXT("continuing_pose_cost_bias")));
	}
	if (Params->HasField(TEXT("base_cost_bias")))
	{
		Database->BaseCostBias = static_cast<float>(Params->GetNumberField(TEXT("base_cost_bias")));
	}
	if (Params->HasField(TEXT("looping_cost_bias")))
	{
		Database->LoopingCostBias = static_cast<float>(Params->GetNumberField(TEXT("looping_cost_bias")));
	}
	if (Params->HasField(TEXT("continuing_interaction_cost_bias")))
	{
		Database->ContinuingInteractionCostBias = static_cast<float>(Params->GetNumberField(TEXT("continuing_interaction_cost_bias")));
	}

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	// Build response — echo back current values
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	FString ModeStr;
	switch (Database->PoseSearchMode)
	{
	case EPoseSearchMode::BruteForce: ModeStr = TEXT("BruteForce"); break;
	case EPoseSearchMode::PCAKDTree:  ModeStr = TEXT("PCAKDTree"); break;
	case EPoseSearchMode::VPTree:     ModeStr = TEXT("VPTree"); break;
	case EPoseSearchMode::EventOnly:  ModeStr = TEXT("EventOnly"); break;
	default:                          ModeStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("pose_search_mode"), ModeStr);
	Root->SetNumberField(TEXT("kd_tree_query_num_neighbors"), Database->KDTreeQueryNumNeighbors);
#if WITH_EDITORONLY_DATA
	Root->SetNumberField(TEXT("number_of_principal_components"), Database->NumberOfPrincipalComponents);
	Root->SetNumberField(TEXT("kd_tree_max_leaf_size"), Database->KDTreeMaxLeafSize);
#endif
	Root->SetNumberField(TEXT("continuing_pose_cost_bias"), Database->ContinuingPoseCostBias);
	Root->SetNumberField(TEXT("base_cost_bias"), Database->BaseCostBias);
	Root->SetNumberField(TEXT("looping_cost_bias"), Database->LoopingCostBias);
	Root->SetNumberField(TEXT("continuing_interaction_cost_bias"), Database->ContinuingInteractionCostBias);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 1.1: create_normalization_set
// (class-member handler; mirrors HandleCreatePoseSearchDatabase)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleCreateNormalizationSet(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	AssetName = AssetPath.Mid(LastSlash + 1);

	if (FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));

	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));

	UPoseSearchNormalizationSet* Set = NewObject<UPoseSearchNormalizationSet>(Pkg, FName(*AssetName), RF_Public | RF_Standalone);
	if (!Set) return FMonolithActionResult::Error(TEXT("Failed to create UPoseSearchNormalizationSet object"));

	FAssetRegistryModule::AssetCreated(Set);
	Pkg->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("asset_path"), Set->GetPathName());
	Root->SetStringField(TEXT("asset_name"), AssetName);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 1.2: add_database_to_normalization_set
// (class-member handler)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleAddDatabaseToNormalizationSet(const TSharedPtr<FJsonObject>& Params)
{
	FString SetPath = Params->GetStringField(TEXT("set_path"));
	FString DatabasePath = Params->GetStringField(TEXT("database_path"));

	UPoseSearchNormalizationSet* Set = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchNormalizationSet>(SetPath);
	if (!Set)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchNormalizationSet not found: %s"), *SetPath));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *DatabasePath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Database To Normalization Set")));
	Set->Modify();

	// Databases is TArray<TObjectPtr<const UPoseSearchDatabase>>; non-const ptr converts implicitly.
	Set->Databases.AddUnique(Database);

	GEditor->EndTransaction();
	Set->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("set_path"), SetPath);
	Root->SetStringField(TEXT("database"), Database->GetPathName());
	Root->SetNumberField(TEXT("count"), Set->Databases.Num());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 1.5: set_database_entry_tags
// (class-member handler; mirrors HandleSetDatabaseSequenceProperties on the unified array)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithPoseSearchActions::HandleSetDatabaseEntryTags(const TSharedPtr<FJsonObject>& Params)
{
	FString DatabasePath = Params->GetStringField(TEXT("database_path"));
	int32 EntryIndex = static_cast<int32>(Params->GetNumberField(TEXT("entry_index")));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *DatabasePath));

	const int32 NumAssets = Database->GetNumAnimationAssets();
	if (EntryIndex < 0 || EntryIndex >= NumAssets)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid entry_index %d (database has %d entries)"), EntryIndex, NumAssets));

	const FPoseSearchDatabaseAnimationAsset* SourceEntry = Database->GetDatabaseAnimationAsset(EntryIndex);
	if (!SourceEntry)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to get entry at index %d"), EntryIndex));

#if WITH_EDITORONLY_DATA
	// Mutate a copy; MonolithCommitDatabaseAnimationAsset writes it back before every exit.
	FPoseSearchDatabaseAnimationAsset EntryCopy = *SourceEntry;
	FPoseSearchDatabaseAnimationAsset* Entry = &EntryCopy;

	GEditor->BeginTransaction(FText::FromString(TEXT("Set PoseSearch Database Entry Tags")));
	Database->Modify();

	if (Params->HasField(TEXT("enabled")))
	{
		Entry->SetIsEnabled(Params->GetBoolField(TEXT("enabled")));
	}

	if (Params->HasField(TEXT("disable_reselection")))
	{
		Entry->SetDisableReselection(Params->GetBoolField(TEXT("disable_reselection")));
	}

	if (Params->HasField(TEXT("mirror_option")))
	{
		FString MirrorStr = Params->GetStringField(TEXT("mirror_option"));
		if (MirrorStr.Equals(TEXT("UnmirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredOnly;
		else if (MirrorStr.Equals(TEXT("MirroredOnly"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::MirroredOnly;
		else if (MirrorStr.Equals(TEXT("UnmirroredAndMirrored"), ESearchCase::IgnoreCase))
			Entry->MirrorOption = EPoseSearchMirrorOption::UnmirroredAndMirrored;
		else
		{
			MonolithCommitDatabaseAnimationAsset(Database, EntryCopy, EntryIndex);
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mirror_option: '%s'. Use UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"), *MirrorStr));
		}
	}

	MonolithCommitDatabaseAnimationAsset(Database, EntryCopy, EntryIndex);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	// Read back current state
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("database_path"), DatabasePath);
	Root->SetNumberField(TEXT("entry_index"), EntryIndex);
	if (UObject* AnimAsset = Entry->GetAnimationAsset())
	{
		Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	}
	Root->SetBoolField(TEXT("enabled"), Entry->IsEnabled());
	Root->SetBoolField(TEXT("disable_reselection"), Entry->IsDisableReselection());
	FString MirrorStr;
	switch (Entry->GetMirrorOption())
	{
	case EPoseSearchMirrorOption::UnmirroredOnly:        MirrorStr = TEXT("UnmirroredOnly"); break;
	case EPoseSearchMirrorOption::MirroredOnly:          MirrorStr = TEXT("MirroredOnly"); break;
	case EPoseSearchMirrorOption::UnmirroredAndMirrored: MirrorStr = TEXT("UnmirroredAndMirrored"); break;
	default:                                             MirrorStr = TEXT("Unknown"); break;
	}
	Root->SetStringField(TEXT("mirror_option"), MirrorStr);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("set_database_entry_tags requires editor-only data (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 1.3: set_database_normalization_set
// (FILE-LOCAL static handler — no header edit)
// ---------------------------------------------------------------------------

static FMonolithActionResult HandleSetDatabaseNormalizationSet(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
	FString DatabasePath = Params->GetStringField(TEXT("database_path"));
	FString SetPath = Params->GetStringField(TEXT("set_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *DatabasePath));

	UPoseSearchNormalizationSet* Set = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchNormalizationSet>(SetPath);
	if (!Set)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchNormalizationSet not found: %s"), *SetPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Set Database Normalization Set")));
	Database->Modify();

	// NormalizationSet is a TObjectPtr<const UPoseSearchNormalizationSet> UPROPERTY with no setter;
	// cast away const on the member to assign.
	const_cast<TObjectPtr<const UPoseSearchNormalizationSet>&>(Database->NormalizationSet) = Set;

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("database_path"), DatabasePath);
	Root->SetStringField(TEXT("normalization_set"), Set->GetPathName());
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("set_database_normalization_set requires editor-only data (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 1.4: add_database_entry
// (FILE-LOCAL static handler — no header edit; mirrors HandleAddDatabaseSequence,
//  uses the NON-DEPRECATED AddAnimationAsset(const FPoseSearchDatabaseAnimationAsset&) overload)
// ---------------------------------------------------------------------------

static FMonolithActionResult HandleAddDatabaseEntry(const TSharedPtr<FJsonObject>& Params)
{
	FString DatabasePath = Params->GetStringField(TEXT("database_path"));
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));

	bool bEnabled = true;
	if (Params->HasField(TEXT("enabled")))
	{
		bEnabled = Params->GetBoolField(TEXT("enabled"));
	}

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *DatabasePath));

	UObject* AnimAsset = FMonolithAssetUtils::LoadAssetByPath<UObject>(AnimPath);
	if (!AnimAsset)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

	if (Database->Contains(AnimAsset))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation already in database: %s"), *AnimPath));

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Database Entry")));
	Database->Modify();

	FPoseSearchDatabaseAnimationAsset NewEntry;
	NewEntry.AnimAsset = AnimAsset;
#if WITH_EDITORONLY_DATA
	NewEntry.bEnabled = bEnabled;

	if (Params->HasField(TEXT("mirror_option")))
	{
		FString MirrorStr = Params->GetStringField(TEXT("mirror_option"));
		if (MirrorStr.Equals(TEXT("UnmirroredOnly"), ESearchCase::IgnoreCase))
			NewEntry.MirrorOption = EPoseSearchMirrorOption::UnmirroredOnly;
		else if (MirrorStr.Equals(TEXT("MirroredOnly"), ESearchCase::IgnoreCase))
			NewEntry.MirrorOption = EPoseSearchMirrorOption::MirroredOnly;
		else if (MirrorStr.Equals(TEXT("UnmirroredAndMirrored"), ESearchCase::IgnoreCase))
			NewEntry.MirrorOption = EPoseSearchMirrorOption::UnmirroredAndMirrored;
		else
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid mirror_option: '%s'. Use UnmirroredOnly, MirroredOnly, or UnmirroredAndMirrored"), *MirrorStr));
		}
	}
#endif

	const int32 IndexBefore = Database->GetNumAnimationAssets();
	// NON-DEPRECATED plain-struct overload (PoseSearchDatabase.h:663) — NOT the FInstancedStruct overload.
	Database->AddAnimationAsset(NewEntry);

	GEditor->EndTransaction();
	Database->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("index"), IndexBefore);
	Root->SetStringField(TEXT("animation"), AnimAsset->GetPathName());
	Root->SetStringField(TEXT("asset_class"), AnimAsset->GetClass()->GetName());
	Root->SetBoolField(TEXT("enabled"), bEnabled);
	Root->SetNumberField(TEXT("count"), Database->GetNumAnimationAssets());
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 2.3: configure_schema_channel
// (FILE-LOCAL static handler — no header edit)
// ---------------------------------------------------------------------------

static FMonolithActionResult HandleConfigureSchemaChannel(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
	FString SchemaPath = Params->GetStringField(TEXT("schema_path"));
	int32 ChannelIndex = static_cast<int32>(Params->GetNumberField(TEXT("channel_index")));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(SchemaPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *SchemaPath));

	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetSchemaChannelsMutable(Schema);
	if (!Channels)
		return FMonolithActionResult::Error(TEXT("Failed to access Channels property on schema via reflection"));

	if (ChannelIndex < 0 || ChannelIndex >= Channels->Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid channel_index %d (schema has %d authored channels)"), ChannelIndex, Channels->Num()));

	UPoseSearchFeatureChannel* Channel = (*Channels)[ChannelIndex];
	if (!Channel)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Channel at index %d is null"), ChannelIndex));

	GEditor->BeginTransaction(FText::FromString(TEXT("Configure PoseSearch Schema Channel")));
	Schema->Modify();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_path"), SchemaPath);
	Root->SetNumberField(TEXT("channel_index"), ChannelIndex);
	Root->SetStringField(TEXT("channel_class"), Channel->GetClass()->GetName());

	const TArray<TSharedPtr<FJsonValue>>* SamplesArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* BonesArr = nullptr;
	const bool bHasSamples = Params->TryGetArrayField(TEXT("samples"), SamplesArr) && SamplesArr;
	const bool bHasBones = Params->TryGetArrayField(TEXT("sampled_bones"), BonesArr) && BonesArr;

	if (UPoseSearchFeatureChannel_Trajectory* TrajChannel = Cast<UPoseSearchFeatureChannel_Trajectory>(Channel))
	{
		if (bHasSamples)
		{
			TrajChannel->Samples.Reset();
			for (const TSharedPtr<FJsonValue>& V : *SamplesArr)
			{
				const TSharedPtr<FJsonObject>* SObj = nullptr;
				if (!V.IsValid() || !V->TryGetObject(SObj) || !SObj) continue;

				FPoseSearchTrajectorySample S;
				double NumVal = 0.0;
				if ((*SObj)->TryGetNumberField(TEXT("offset"), NumVal)) S.Offset = static_cast<float>(NumVal);
				if ((*SObj)->TryGetNumberField(TEXT("flags"), NumVal)) S.Flags = static_cast<int32>(NumVal);
				if ((*SObj)->TryGetNumberField(TEXT("weight"), NumVal)) S.Weight = static_cast<float>(NumVal);
				TrajChannel->Samples.Add(S);
			}
		}
		if (Params->HasField(TEXT("weight")))
		{
			TrajChannel->Weight = static_cast<float>(Params->GetNumberField(TEXT("weight")));
		}
		Root->SetNumberField(TEXT("sample_count"), TrajChannel->Samples.Num());
	}
	else if (UPoseSearchFeatureChannel_Pose* PoseChannel = Cast<UPoseSearchFeatureChannel_Pose>(Channel))
	{
		if (bHasBones)
		{
			PoseChannel->SampledBones.Reset();
			for (const TSharedPtr<FJsonValue>& V : *BonesArr)
			{
				FString BoneName;
				if (!V.IsValid() || !V->TryGetString(BoneName) || BoneName.IsEmpty()) continue;
				FPoseSearchBone B;
				B.Reference.BoneName = FName(*BoneName);
				PoseChannel->SampledBones.Add(B);
			}
		}
		if (Params->HasField(TEXT("weight")))
		{
			PoseChannel->Weight = static_cast<float>(Params->GetNumberField(TEXT("weight")));
		}
		Root->SetNumberField(TEXT("bone_count"), PoseChannel->SampledBones.Num());
	}
	else
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Channel at index %d is class %s; only Trajectory (samples) and Pose (sampled_bones) channels are configurable"),
			ChannelIndex, *Channel->GetClass()->GetName()));
	}

	// Re-finalize so cardinality/data offsets reflect the new sample/bone counts.
	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("configure_schema_channel requires editor-only data (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 2.4: add_pose_search_notify
// (FILE-LOCAL static handler — no header edit; reuses AnimationBlueprintLibrary helper)
// ---------------------------------------------------------------------------

/** Map a notify_kind string to the concrete UAnimNotifyState_PoseSearch* UClass.
  * 8 concrete classes exist in UE 5.7. Returns nullptr if unrecognized. */
static UClass* ResolvePoseSearchNotifyClass(const FString& Kind)
{
	if (Kind.Equals(TEXT("ExcludeFromDatabase"), ESearchCase::IgnoreCase)) return UAnimNotifyState_PoseSearchExcludeFromDatabase::StaticClass();
	if (Kind.Equals(TEXT("BlockTransition"), ESearchCase::IgnoreCase))     return UAnimNotifyState_PoseSearchBlockTransition::StaticClass();
	if (Kind.Equals(TEXT("ModifyCost"), ESearchCase::IgnoreCase))          return UAnimNotifyState_PoseSearchModifyCost::StaticClass();
	if (Kind.Equals(TEXT("OverrideContinuingPoseCostBias"), ESearchCase::IgnoreCase)) return UAnimNotifyState_PoseSearchOverrideContinuingPoseCostBias::StaticClass();
	if (Kind.Equals(TEXT("SamplingEvent"), ESearchCase::IgnoreCase))       return UAnimNotifyState_PoseSearchSamplingEvent::StaticClass();
	if (Kind.Equals(TEXT("SamplingAttribute"), ESearchCase::IgnoreCase))   return UAnimNotifyState_PoseSearchSamplingAttribute::StaticClass();
	if (Kind.Equals(TEXT("BranchIn"), ESearchCase::IgnoreCase))            return UAnimNotifyState_PoseSearchBranchIn::StaticClass();
#if !(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	// UAnimNotifyState_PoseSearchIKWindow was removed in UE 5.8.
	if (Kind.Equals(TEXT("IKWindow"), ESearchCase::IgnoreCase))            return UAnimNotifyState_PoseSearchIKWindow::StaticClass();
#endif
	return nullptr;
}

static FMonolithActionResult HandleAddPoseSearchNotify(const TSharedPtr<FJsonObject>& Params)
{
	FString AnimPath = Params->GetStringField(TEXT("anim_path"));
	FString NotifyKind = Params->GetStringField(TEXT("notify_kind"));
	float StartTime = static_cast<float>(Params->GetNumberField(TEXT("start_time")));
	float Duration = static_cast<float>(Params->GetNumberField(TEXT("duration")));

	UAnimSequenceBase* Seq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequenceBase>(AnimPath);
	if (!Seq)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation asset not found: %s"), *AnimPath));

	if (StartTime < 0.f || StartTime > Seq->GetPlayLength())
		return FMonolithActionResult::Error(FString::Printf(TEXT("start_time %.3f out of range [0, %.3f]"), StartTime, Seq->GetPlayLength()));
	if (Duration <= 0.f)
		return FMonolithActionResult::Error(TEXT("duration must be > 0"));
	if (StartTime + Duration > Seq->GetPlayLength())
		return FMonolithActionResult::Error(FString::Printf(TEXT("start_time + duration (%.3f) exceeds play length (%.3f)"), StartTime + Duration, Seq->GetPlayLength()));

	UClass* NotifyClass = ResolvePoseSearchNotifyClass(NotifyKind);
	if (!NotifyClass)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown notify_kind '%s'. Supported: ExcludeFromDatabase, BlockTransition, ModifyCost, OverrideContinuingPoseCostBias, SamplingEvent, SamplingAttribute, BranchIn, IKWindow"), *NotifyKind));

	UAnimNotifyState* NewNotify = NewObject<UAnimNotifyState>(Seq, NotifyClass);
	if (!NewNotify)
		return FMonolithActionResult::Error(TEXT("Failed to create PoseSearch notify-state instance"));

	// Kind-specific params: CostAddend on the two cost-bias notifies.
	if (Params->HasField(TEXT("cost_bias")))
	{
		float CostBias = static_cast<float>(Params->GetNumberField(TEXT("cost_bias")));
		if (UAnimNotifyState_PoseSearchModifyCost* ModifyCost = Cast<UAnimNotifyState_PoseSearchModifyCost>(NewNotify))
		{
			ModifyCost->CostAddend = CostBias;
		}
		else if (UAnimNotifyState_PoseSearchOverrideContinuingPoseCostBias* OverrideCost = Cast<UAnimNotifyState_PoseSearchOverrideContinuingPoseCostBias>(NewNotify))
		{
			OverrideCost->CostAddend = CostBias;
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Add PoseSearch Notify")));
	Seq->Modify();

	// The "PoseSearch" track does not exist on a fresh sequence; AddAnimationNotifyStateEventObject
	// silently no-ops on an unknown track name. Ensure it exists first (this call no-ops if present).
	UAnimationBlueprintLibrary::AddAnimationNotifyTrack(Seq, FName(TEXT("PoseSearch")), FLinearColor::White);
	UAnimationBlueprintLibrary::AddAnimationNotifyStateEventObject(Seq, StartTime, Duration, NewNotify, FName(TEXT("PoseSearch")));
	Seq->RefreshCacheData();

	GEditor->EndTransaction();
	Seq->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("anim_path"), AnimPath);
	Root->SetStringField(TEXT("notify_kind"), NotifyKind);
	Root->SetStringField(TEXT("notify_class"), NotifyClass->GetName());
	Root->SetNumberField(TEXT("start_time"), StartTime);
	Root->SetNumberField(TEXT("duration"), Duration);
	Root->SetNumberField(TEXT("index"), Seq->Notifies.Num() - 1);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 2.5: derive_schema_channels_from_skeleton
// (FILE-LOCAL static handler — no header edit; depends on 2.3 channel internals)
// ---------------------------------------------------------------------------

static FMonolithActionResult HandleDeriveSchemaChannelsFromSkeleton(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITORONLY_DATA
	FString SchemaPath = Params->GetStringField(TEXT("schema_path"));
	FString SkeletonPath = Params->GetStringField(TEXT("skeleton_path"));

	UPoseSearchSchema* Schema = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchSchema>(SchemaPath);
	if (!Schema)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchSchema not found: %s"), *SchemaPath));

	USkeleton* Skeleton = FMonolithAssetUtils::LoadAssetByPath<USkeleton>(SkeletonPath);
	if (!Skeleton)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));

	// Heuristically pick foot bones: name matches foot/ball/toe AND a left/right marker.
	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	TArray<FName> FootBones;
	for (int32 b = 0; b < RefSkel.GetNum(); ++b)
	{
		FString BoneName = RefSkel.GetBoneName(b).ToString();
		const bool bIsFootLike =
			BoneName.Contains(TEXT("foot"), ESearchCase::IgnoreCase) ||
			BoneName.Contains(TEXT("ball"), ESearchCase::IgnoreCase) ||
			BoneName.Contains(TEXT("toe"), ESearchCase::IgnoreCase);
		const bool bHasSide =
			BoneName.Contains(TEXT("_l"), ESearchCase::IgnoreCase) ||
			BoneName.Contains(TEXT("_r"), ESearchCase::IgnoreCase) ||
			BoneName.Contains(TEXT("left"), ESearchCase::IgnoreCase) ||
			BoneName.Contains(TEXT("right"), ESearchCase::IgnoreCase) ||
			BoneName.StartsWith(TEXT("l_"), ESearchCase::IgnoreCase) ||
			BoneName.StartsWith(TEXT("r_"), ESearchCase::IgnoreCase);
		if (bIsFootLike && bHasSide)
		{
			FootBones.AddUnique(RefSkel.GetBoneName(b));
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("Derive Schema Channels From Skeleton")));
	Schema->Modify();

	// Reuse the default Pose channel if AddDefaultChannels() already seeded one; otherwise
	// creating a second Pose channel duplicates its bones (e.g. foot_l/foot_r) on coalesce.
	UPoseSearchFeatureChannel_Pose* PoseChannel = nullptr;
	if (TArray<TObjectPtr<UPoseSearchFeatureChannel>>* ExistingChannels = GetSchemaChannelsMutable(Schema))
	{
		for (const TObjectPtr<UPoseSearchFeatureChannel>& Ch : *ExistingChannels)
		{
			if (UPoseSearchFeatureChannel_Pose* ExistingPose = Cast<UPoseSearchFeatureChannel_Pose>(Ch))
			{
				PoseChannel = ExistingPose;
				break;
			}
		}
	}

	const bool bReusedPoseChannel = (PoseChannel != nullptr);
	if (bReusedPoseChannel)
	{
		// Reuse: clear and repopulate so only one Pose channel exists with deduped bones.
		PoseChannel->SampledBones.Reset();
	}
	else
	{
		PoseChannel = NewObject<UPoseSearchFeatureChannel_Pose>(Schema, UPoseSearchFeatureChannel_Pose::StaticClass(), NAME_None, RF_Transactional);
	}

	for (const FName& Bone : FootBones)
	{
		FPoseSearchBone B;
		B.Reference.BoneName = Bone;
		// Position + Velocity is the standard foot-bone feature set for locomotion MM.
		B.Flags = int32(EPoseSearchBoneFlags::Position) | int32(EPoseSearchBoneFlags::Velocity);
		PoseChannel->SampledBones.Add(B);
	}

	// Only attach a freshly created channel; a reused one is already in the schema's array.
	if (!bReusedPoseChannel)
	{
		Schema->AddChannel(PoseChannel);
	}

	// Reuse the default Trajectory channel if AddDefaultChannels() already seeded one; otherwise
	// adding a second Trajectory channel double-weights trajectory (same defect class as Pose).
	UPoseSearchFeatureChannel_Trajectory* TrajChannel = nullptr;
	if (TArray<TObjectPtr<UPoseSearchFeatureChannel>>* ExistingChannels = GetSchemaChannelsMutable(Schema))
	{
		for (const TObjectPtr<UPoseSearchFeatureChannel>& Ch : *ExistingChannels)
		{
			if (UPoseSearchFeatureChannel_Trajectory* ExistingTraj = Cast<UPoseSearchFeatureChannel_Trajectory>(Ch))
			{
				TrajChannel = ExistingTraj;
				break;
			}
		}
	}

	const bool bReusedTrajChannel = (TrajChannel != nullptr);
	if (bReusedTrajChannel)
	{
		// Reuse: overwrite the default sample set so only one Trajectory channel exists.
		TrajChannel->Samples.Reset();
	}
	else
	{
		TrajChannel = NewObject<UPoseSearchFeatureChannel_Trajectory>(Schema, UPoseSearchFeatureChannel_Trajectory::StaticClass(), NAME_None, RF_Transactional);
	}

	auto MakeSample = [](float Offset, EPoseSearchTrajectoryFlags Flags)
	{
		FPoseSearchTrajectorySample S;
		S.Offset = Offset;
		S.Flags = int32(Flags);
		return S;
	};
	TrajChannel->Samples.Add(MakeSample(-0.4f, EPoseSearchTrajectoryFlags::Position));
	TrajChannel->Samples.Add(MakeSample(0.0f,  EPoseSearchTrajectoryFlags::Velocity));
	TrajChannel->Samples.Add(MakeSample(0.4f,  EPoseSearchTrajectoryFlags::Position));
	TrajChannel->Samples.Add(MakeSample(0.7f,  EPoseSearchTrajectoryFlags::FacingDirection));

	// Only attach a freshly created channel; a reused one is already in the schema's array.
	if (!bReusedTrajChannel)
	{
		Schema->AddChannel(TrajChannel);
	}

	TriggerSchemaFinalize(Schema);

	GEditor->EndTransaction();
	Schema->MarkPackageDirty();

	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* AuthoredChannels = GetSchemaChannelsMutable(Schema);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema_path"), SchemaPath);
	Root->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Root->SetNumberField(TEXT("foot_bone_count"), FootBones.Num());
	{
		TArray<TSharedPtr<FJsonValue>> BoneArr;
		for (const FName& Bone : FootBones) BoneArr.Add(MakeShared<FJsonValueString>(Bone.ToString()));
		Root->SetArrayField(TEXT("foot_bones"), BoneArr);
	}
	Root->SetNumberField(TEXT("authored_channel_count"), AuthoredChannels ? AuthoredChannels->Num() : -1);
	Root->SetNumberField(TEXT("trajectory_sample_count"), TrajChannel->Samples.Num());
	Root->SetNumberField(TEXT("schema_cardinality"), Schema->SchemaCardinality);
	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("derive_schema_channels_from_skeleton requires editor-only data (WITH_EDITORONLY_DATA)"));
#endif
}

// ---------------------------------------------------------------------------
// Motion Matching Pack — Task 2.6: validate_pose_search_database
// (FILE-LOCAL static handler — no header edit; NO mutation)
// ---------------------------------------------------------------------------

static FMonolithActionResult HandleValidatePoseSearchDatabase(const TSharedPtr<FJsonObject>& Params)
{
	FString DatabasePath = Params->GetStringField(TEXT("database_path"));

	UPoseSearchDatabase* Database = FMonolithAssetUtils::LoadAssetByPath<UPoseSearchDatabase>(DatabasePath);
	if (!Database)
		return FMonolithActionResult::Error(FString::Printf(TEXT("PoseSearchDatabase not found: %s"), *DatabasePath));

	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bValid = true;

	// Schema presence.
	if (!Database->Schema)
	{
		bValid = false;
		Issues.Add(MakeShared<FJsonValueString>(TEXT("Database has no Schema assigned")));
	}

	// Per-entry skeleton compatibility (editor-only API).
	const int32 NumEntries = Database->GetNumAnimationAssets();
#if WITH_EDITOR
	if (Database->Schema)
	{
		for (int32 i = 0; i < NumEntries; ++i)
		{
			const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(i);
			if (!Entry) continue;
			if (!Entry->IsSkeletonCompatible(Database->Schema))
			{
				bValid = false;
				FString AnimName = Entry->GetAnimationAsset() ? Entry->GetAnimationAsset()->GetPathName() : TEXT("<null>");
				Issues.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("Entry %d (%s) skeleton incompatible with schema"), i, *AnimName)));
			}
		}
	}
#endif

	// Stale-index detection: ContinueRequest ensures associated data exists WITHOUT forcing a new key.
	// Non-Success result => index stale / not built. NO mutation (no NewRequest).
	bool bStaleIndex = true;
#if WITH_EDITOR
	{
		using namespace UE::PoseSearch;
		// ContinueRequest ensures associated data WITHOUT forcing a new key (no rebuild/mutation).
		// WaitForCompletion mirrors the shipped get_database_stats guard so GetSearchIndex() below
		// (which check()-asserts on an un-built DB) is only touched once a build has settled.
		const EAsyncBuildIndexResult IndexResult = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(
			Database, ERequestAsyncBuildFlag::ContinueRequest | ERequestAsyncBuildFlag::WaitForCompletion);
		bStaleIndex = (IndexResult != EAsyncBuildIndexResult::Success);

		// Cross-check pose count ONLY if the index is built (GetSearchIndex check()-asserts otherwise).
		if (!bStaleIndex)
		{
			const int32 NumPoses = Database->GetSearchIndex().GetNumPoses();
			if (NumPoses <= 0)
			{
				bValid = false;
				Issues.Add(MakeShared<FJsonValueString>(TEXT("Search index reports zero poses")));
			}
		}
		else
		{
			Issues.Add(MakeShared<FJsonValueString>(TEXT("Search index is stale or not built (rebuild_pose_search_index required)")));
		}
	}
#endif

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("database_path"), DatabasePath);
	Root->SetBoolField(TEXT("valid"), bValid);
	Root->SetBoolField(TEXT("stale_index"), bStaleIndex);
	Root->SetNumberField(TEXT("entry_count"), NumEntries);
	Root->SetArrayField(TEXT("issues"), Issues);
	if (Database->Schema)
	{
		Root->SetStringField(TEXT("schema"), Database->Schema->GetPathName());
	}
	return FMonolithActionResult::Success(Root);
}
