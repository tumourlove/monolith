#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FMonolithActionResult;
class FMonolithToolRegistry;

class FMonolithAudioBatchActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult BatchAssignSoundClass(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchAssignAttenuation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetCompression(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetSubmix(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetConcurrency(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetLooping(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetVirtualization(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchRenameAudio(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchSetSoundWaveProperties(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ApplyAudioTemplate(const TSharedPtr<FJsonObject>& Params);
};
