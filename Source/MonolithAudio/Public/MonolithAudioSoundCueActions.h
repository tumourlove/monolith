#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FMonolithToolRegistry;
struct FMonolithActionResult;
class FJsonObject;
class USoundCue;
class USoundNode;

class FMonolithAudioSoundCueActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	// --- CRUD (10) ---
	static FMonolithActionResult CreateSoundCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSoundCueGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddSoundCueNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveSoundCueNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ConnectSoundCueNodes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSoundCueFirstNode(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetSoundCueNodeProperty(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListSoundCueNodeTypes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindSoundWavesInCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateSoundCue(const TSharedPtr<FJsonObject>& Params);

	// --- Build & Templates (8) ---
	static FMonolithActionResult BuildSoundCueFromSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateRandomSoundCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateLayeredSoundCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateLoopingAmbientCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateDistanceCrossfadeCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateSwitchSoundCue(const TSharedPtr<FJsonObject>& Params);

	// --- Utility (5) ---
	static FMonolithActionResult DuplicateSoundCue(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DeleteAudioAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PreviewSound(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult StopPreview(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetSoundCueDuration(const TSharedPtr<FJsonObject>& Params);

	// --- Helpers ---

	/** Get the node type registry: string name -> UClass* */
	static const TMap<FString, UClass*>& GetNodeTypeRegistry();

	/** Resolve a node type string to a UClass* */
	static UClass* ResolveNodeType(const FString& TypeName);

	/** Build a node ID string from a USoundNode (index-based) */
	static FString MakeNodeId(USoundCue* Cue, USoundNode* Node);

	/** Find a node by ID string within a cue */
	static USoundNode* FindNodeById(USoundCue* Cue, const FString& NodeId);

	/** Serialize a single sound node to JSON */
	static TSharedPtr<FJsonObject> SerializeNode(USoundCue* Cue, USoundNode* Node);

	/** Set a property on a sound node via reflection (with SoundWave special case) */
	static bool SetNodeProperty(USoundNode* Node, const FString& PropName, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/** Load a USoundCue by path */
	static USoundCue* LoadSoundCue(const FString& AssetPath, FString& OutError);

	/** Finalize a sound cue after graph modifications */
	static void FinalizeCue(USoundCue* Cue);

	/** Create a sound cue package + empty cue object */
	static USoundCue* CreateEmptySoundCue(const FString& AssetPath, FString& OutError);

	/** Create WavePlayer nodes from an array of wave paths, return the nodes */
	static TArray<USoundNode*> CreateWavePlayerNodes(USoundCue* Cue, const TArray<FString>& WavePaths, FString& OutError);
};
