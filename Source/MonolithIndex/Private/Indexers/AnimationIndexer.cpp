#include "Indexers/AnimationIndexer.h"
#include "MonolithSettings.h"
#include "MonolithMemoryHelper.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "Animation/PoseAsset.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

// Isolated SEH wrapper — can't use __try in functions with C++ objects
static bool SafeCallWithSEH(void(*Func)(void*), void* Context)
{
	__try
	{
		Func(Context);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
#endif

/** Context struct for SEH-safe load+index calls */
struct FAnimIndexCallContext
{
	FAnimationIndexer* Self;
	FMonolithIndexDatabase* DB;
	int64 AssetId;
	FSoftObjectPath ObjectPath;  // Load happens inside SEH guard
	bool bSuccess;               // Set to true if load+index succeeded
	enum EType { Sequence, Montage, BlendSp, Pose } Type;
};

static void LoadAndIndexAnimAsset(void* Ctx)
{
	auto* C = static_cast<FAnimIndexCallContext*>(Ctx);
	C->bSuccess = false;

	// TryLoad inside SEH guard — this is where the crash was happening
	UObject* Loaded = C->ObjectPath.TryLoad();
	if (!Loaded) return;

	switch (C->Type)
	{
	case FAnimIndexCallContext::Sequence:
		if (UAnimSequence* A = Cast<UAnimSequence>(Loaded))
		{ C->Self->IndexAnimSequencePublic(A, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	case FAnimIndexCallContext::Montage:
		if (UAnimMontage* A = Cast<UAnimMontage>(Loaded))
		{ C->Self->IndexAnimMontagePublic(A, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	case FAnimIndexCallContext::BlendSp:
		if (UBlendSpace* A = Cast<UBlendSpace>(Loaded))
		{ C->Self->IndexBlendSpacePublic(A, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	case FAnimIndexCallContext::Pose:
		if (UPoseAsset* A = Cast<UPoseAsset>(Loaded))
		{ C->Self->IndexPoseAssetPublic(A, *C->DB, C->AssetId); C->bSuccess = true; }
		break;
	}
}

bool FAnimationIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Get settings for batching
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	const int32 BatchSize = FMath::Max(1, Settings->PostPassBatchSize);
	const SIZE_T MemoryBudgetMB = static_cast<SIZE_T>(Settings->MemoryBudgetMB);
	const bool bLogMemory = Settings->bLogMemoryStats;

	int32 TotalIndexed = 0;

	// Helper lambda: scan registry for type T, safely load and index each asset with batching and GC
	auto IndexAllOfType = [&](UClass* Class, FAnimIndexCallContext::EType Type, const TCHAR* TypeName) -> int32
	{
		TArray<FAssetData> Assets;
		FARFilter Filter;
		for (const FName& ContentPath : UMonolithSettings::GetIndexedContentPaths())
		{
			Filter.PackagePaths.Add(ContentPath);
		}
		Filter.bRecursivePaths = true;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Registry.GetAssets(Filter, Assets);

		if (Assets.Num() == 0) return 0;

		UE_LOG(LogMonolithIndex, Log, TEXT("AnimationIndexer: Processing %d %s assets"), Assets.Num(), TypeName);

		int32 Count = 0;
		int32 BatchNumber = 0;

		for (int32 i = 0; i < Assets.Num(); i += BatchSize)
		{
			// Memory budget check before each batch
			if (FMonolithMemoryHelper::ShouldThrottle(MemoryBudgetMB))
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("AnimationIndexer: Memory budget exceeded, forcing GC..."));
				FMonolithMemoryHelper::ForceGarbageCollection(true);
				FMonolithMemoryHelper::YieldToEditor();
			}

			int32 BatchEnd = FMath::Min(i + BatchSize, Assets.Num());

			// Process batch
			for (int32 j = i; j < BatchEnd; ++j)
			{
				const FAssetData& AD = Assets[j];

				int64 AId = DB.GetAssetId(AD.PackageName.ToString());
				if (AId < 0) continue;

				FAnimIndexCallContext Ctx;
				Ctx.Self = this;
				Ctx.DB = &DB;
				Ctx.AssetId = AId;
				Ctx.ObjectPath = AD.GetSoftObjectPath();
				Ctx.bSuccess = false;
				Ctx.Type = Type;

#if PLATFORM_WINDOWS
				if (SafeCallWithSEH(&LoadAndIndexAnimAsset, &Ctx))
				{
					if (Ctx.bSuccess) Count++;
				}
				else
				{
					UE_LOG(LogMonolithIndex, Warning, TEXT("AnimationIndexer: crashed loading/indexing '%s' - skipping"), *AD.GetSoftObjectPath().ToString());
				}
#else
				LoadAndIndexAnimAsset(&Ctx);
				if (Ctx.bSuccess) Count++;
#endif
			}

			BatchNumber++;

			// GC after each batch
			FMonolithMemoryHelper::ForceGarbageCollection(false);
			FMonolithMemoryHelper::YieldToEditor();

			// Log progress periodically
			if (BatchNumber % 10 == 0 || BatchEnd == Assets.Num())
			{
				UE_LOG(LogMonolithIndex, Log, TEXT("AnimationIndexer: %s progress %d / %d"), TypeName, BatchEnd, Assets.Num());
			}
		}

		return Count;
	};

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("AnimationIndexer start"));
	}

	TotalIndexed += IndexAllOfType(UAnimSequence::StaticClass(), FAnimIndexCallContext::Sequence, TEXT("AnimSequence"));
	TotalIndexed += IndexAllOfType(UAnimMontage::StaticClass(), FAnimIndexCallContext::Montage, TEXT("AnimMontage"));
	TotalIndexed += IndexAllOfType(UBlendSpace::StaticClass(), FAnimIndexCallContext::BlendSp, TEXT("BlendSpace"));
	TotalIndexed += IndexAllOfType(UPoseAsset::StaticClass(), FAnimIndexCallContext::Pose, TEXT("PoseAsset"));

	// Final GC
	FMonolithMemoryHelper::ForceGarbageCollection(true);

	UE_LOG(LogMonolithIndex, Log, TEXT("AnimationIndexer: indexed %d animation assets"), TotalIndexed);

	if (bLogMemory)
	{
		FMonolithMemoryHelper::LogMemoryStats(TEXT("AnimationIndexer complete"));
	}

	return true;
}

void FAnimationIndexer::IndexAnimSequence(UAnimSequence* AnimSeq, FMonolithIndexDatabase& DB, int64 AssetId)
{
	USkeleton* Skeleton = AnimSeq->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	// Build properties JSON
	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);
	Props->SetNumberField(TEXT("length"), AnimSeq->GetPlayLength());
	Props->SetNumberField(TEXT("num_frames"), AnimSeq->GetNumberOfSampledKeys());
	Props->SetNumberField(TEXT("rate_scale"), AnimSeq->RateScale);

	// Bone tracks
	TArray<TSharedPtr<FJsonValue>> TracksArr;
	if (Skeleton)
	{
		const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
		const int32 NumBones = RefSkeleton.GetNum();
		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			TracksArr.Add(MakeShared<FJsonValueString>(RefSkeleton.GetBoneName(BoneIdx).ToString()));
		}
	}
	Props->SetArrayField(TEXT("bone_tracks"), TracksArr);

	// Curves
	TArray<TSharedPtr<FJsonValue>> CurvesArr;
	const FRawCurveTracks& RawCurves = AnimSeq->GetCurveData();
	for (const FFloatCurve& Curve : RawCurves.FloatCurves)
	{
		auto CurveObj = MakeShared<FJsonObject>();
		CurveObj->SetStringField(TEXT("name"), Curve.GetName().ToString());
		CurveObj->SetNumberField(TEXT("num_keys"), Curve.FloatCurve.GetNumKeys());
		CurvesArr.Add(MakeShared<FJsonValueObject>(CurveObj));
	}
	Props->SetArrayField(TEXT("curves"), CurvesArr);

	// Notifies
	Props->SetStringField(TEXT("notifies"), NotifiesToJson(AnimSeq->Notifies));

	// Serialize properties
	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeType = TEXT("AnimSequence");
	Node.NodeName = AnimSeq->GetName();
	Node.NodeClass = TEXT("UAnimSequence");
	Node.Properties = PropsStr;
	DB.InsertNode(Node);
}

void FAnimationIndexer::IndexAnimMontage(UAnimMontage* Montage, FMonolithIndexDatabase& DB, int64 AssetId)
{
	USkeleton* Skeleton = Montage->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);
	Props->SetNumberField(TEXT("length"), Montage->GetPlayLength());

	// Sections
	TArray<TSharedPtr<FJsonValue>> SectionsArr;
	for (const FCompositeSection& Section : Montage->CompositeSections)
	{
		auto SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("name"), Section.SectionName.ToString());
		SectionObj->SetNumberField(TEXT("start_time"), Section.GetTime());
		SectionObj->SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
		SectionsArr.Add(MakeShared<FJsonValueObject>(SectionObj));
	}
	Props->SetArrayField(TEXT("sections"), SectionsArr);

	// Slots
	TArray<TSharedPtr<FJsonValue>> SlotsArr;
	for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
	{
		auto SlotObj = MakeShared<FJsonObject>();
		SlotObj->SetStringField(TEXT("name"), Slot.SlotName.ToString());
		SlotObj->SetNumberField(TEXT("num_segments"), Slot.AnimTrack.AnimSegments.Num());
		SlotsArr.Add(MakeShared<FJsonValueObject>(SlotObj));
	}
	Props->SetArrayField(TEXT("slots"), SlotsArr);

	// Notifies
	Props->SetStringField(TEXT("notifies"), NotifiesToJson(Montage->Notifies));

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeType = TEXT("AnimMontage");
	Node.NodeName = Montage->GetName();
	Node.NodeClass = TEXT("UAnimMontage");
	Node.Properties = PropsStr;
	DB.InsertNode(Node);
}

void FAnimationIndexer::IndexBlendSpace(UBlendSpace* BlendSpace, FMonolithIndexDatabase& DB, int64 AssetId)
{
	USkeleton* Skeleton = BlendSpace->GetSkeleton();
	const FString SkeletonName = Skeleton ? Skeleton->GetPathName() : TEXT("None");

	auto Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("skeleton"), SkeletonName);

	// Axes
	const FBlendParameter& AxisX = BlendSpace->GetBlendParameter(0);
	const FBlendParameter& AxisY = BlendSpace->GetBlendParameter(1);

	auto AxisXObj = MakeShared<FJsonObject>();
	AxisXObj->SetStringField(TEXT("name"), AxisX.DisplayName);
	AxisXObj->SetNumberField(TEXT("min"), AxisX.Min);
	AxisXObj->SetNumberField(TEXT("max"), AxisX.Max);
	AxisXObj->SetNumberField(TEXT("grid_num"), AxisX.GridNum);
	Props->SetObjectField(TEXT("axis_x"), AxisXObj);

	auto AxisYObj = MakeShared<FJsonObject>();
	AxisYObj->SetStringField(TEXT("name"), AxisY.DisplayName);
	AxisYObj->SetNumberField(TEXT("min"), AxisY.Min);
	AxisYObj->SetNumberField(TEXT("max"), AxisY.Max);
	AxisYObj->SetNumberField(TEXT("grid_num"), AxisY.GridNum);
	Props->SetObjectField(TEXT("axis_y"), AxisYObj);

	// Sample points
	TArray<TSharedPtr<FJsonValue>> SamplesArr;
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
	for (const FBlendSample& Sample : Samples)
	{
		auto SampleObj = MakeShared<FJsonObject>();
		SampleObj->SetStringField(TEXT("animation"), Sample.Animation ? Sample.Animation->GetPathName() : TEXT("None"));
		SampleObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
		SampleObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
		SampleObj->SetNumberField(TEXT("rate_scale"), Sample.RateScale);
		SamplesArr.Add(MakeShared<FJsonValueObject>(SampleObj));
	}
	Props->SetArrayField(TEXT("sample_points"), SamplesArr);

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeType = TEXT("BlendSpace");
	Node.NodeName = BlendSpace->GetName();
	Node.NodeClass = TEXT("UBlendSpace");
	Node.Properties = PropsStr;
	DB.InsertNode(Node);
}

void FAnimationIndexer::IndexPoseAsset(UPoseAsset* PoseAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!PoseAsset) return;

	auto Props = MakeShared<FJsonObject>();

	if (PoseAsset->GetSkeleton())
	{
		Props->SetStringField(TEXT("skeleton"), PoseAsset->GetSkeleton()->GetPathName());
	}

	Props->SetNumberField(TEXT("num_poses"), PoseAsset->GetNumPoses());
	Props->SetNumberField(TEXT("num_tracks"), PoseAsset->GetNumTracks());
	Props->SetNumberField(TEXT("num_curves"), PoseAsset->GetNumCurves());
	Props->SetBoolField(TEXT("is_additive"), PoseAsset->IsValidAdditive());

	if (!PoseAsset->RetargetSource.IsNone())
	{
		Props->SetStringField(TEXT("retarget_source"), PoseAsset->RetargetSource.ToString());
	}

	// Pose names (using GetPoseFNames — GetPoseNames is deprecated since 5.3)
	const TArray<FName>& PoseNames = PoseAsset->GetPoseFNames();
	TArray<TSharedPtr<FJsonValue>> PoseNameArray;
	for (const FName& Name : PoseNames)
	{
		PoseNameArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	Props->SetArrayField(TEXT("pose_names"), PoseNameArray);

	FString PropsStr;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
	FJsonSerializer::Serialize(Props, *Writer, true);

	FIndexedNode PoseNode;
	PoseNode.AssetId = AssetId;
	PoseNode.NodeName = PoseAsset->GetName();
	PoseNode.NodeClass = TEXT("PoseAsset");
	PoseNode.NodeType = TEXT("PoseAsset");
	PoseNode.Properties = PropsStr;
	DB.InsertNode(PoseNode);
}

FString FAnimationIndexer::NotifiesToJson(const TArray<FAnimNotifyEvent>& Notifies)
{
	TArray<TSharedPtr<FJsonValue>> NotifyArr;
	for (const FAnimNotifyEvent& Notify : Notifies)
	{
		auto NotifyObj = MakeShared<FJsonObject>();
		NotifyObj->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
		NotifyObj->SetNumberField(TEXT("trigger_time"), Notify.GetTriggerTime());
		NotifyObj->SetNumberField(TEXT("duration"), Notify.GetDuration());

		if (Notify.Notify)
		{
			NotifyObj->SetStringField(TEXT("class"), Notify.Notify->GetClass()->GetName());
		}
		else if (Notify.NotifyStateClass)
		{
			NotifyObj->SetStringField(TEXT("class"), Notify.NotifyStateClass->GetClass()->GetName());
			NotifyObj->SetBoolField(TEXT("is_state"), true);
		}

		NotifyArr.Add(MakeShared<FJsonValueObject>(NotifyObj));
	}

	FString Result;
	auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(NotifyArr, *Writer);
	return Result;
}
