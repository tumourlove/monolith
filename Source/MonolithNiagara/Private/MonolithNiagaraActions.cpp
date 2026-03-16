#include "MonolithNiagaraActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "NiagaraDataInterfaceVector4Curve.h"
#include "Curves/RichCurve.h"
#include "NiagaraConstants.h"
#include "NiagaraTypes.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraComponentRendererProperties.h"
#include "NiagaraEditorModule.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraCommon.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraShared.h"
// NiagaraEffectType.h — needed for UNiagaraEffectType (SetEffectType, GetEffectType)
// Forward-declared in NiagaraSystem.h; full definition needed for LoadObject<>
#include "NiagaraEffectType.h"
#include "NiagaraSimulationStageBase.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithNiagara, Log, All);

// ============================================================================
// Workarounds for non-exported NiagaraEditor functions
// These functions exist in NiagaraStackGraphUtilities but lack NIAGARAEDITOR_API
// ============================================================================

namespace MonolithNiagaraHelpers
{
	// Helper: find the ParameterMap pin on a node (matches engine's GetParameterMapPin logic)
	UEdGraphPin* GetParameterMapPin(UNiagaraNode& Node, EEdGraphPinDirection Direction)
	{
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin->Direction == Direction)
			{
				FNiagaraTypeDefinition PinDef = Schema->PinToTypeDefinition(Pin);
				if (PinDef == FNiagaraTypeDefinition::GetParameterMapDef())
				{
					return Pin;
				}
			}
		}
		return nullptr;
	}

	// Mirrors engine's FNiagaraStackGraphUtilities::GetOrderedModuleNodes — walks ParameterMap pins
	void GetOrderedModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
	{
		OutModuleNodes.Reset();
		UNiagaraNode* PreviousNode = &OutputNode;
		while (PreviousNode != nullptr)
		{
			UEdGraphPin* PrevInputPin = GetParameterMapPin(*PreviousNode, EGPD_Input);
			if (PrevInputPin != nullptr && PrevInputPin->LinkedTo.Num() == 1)
			{
				UNiagaraNode* CurrentNode = Cast<UNiagaraNode>(PrevInputPin->LinkedTo[0]->GetOwningNode());
				UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(CurrentNode);
				if (ModuleNode != nullptr)
				{
					OutModuleNodes.Insert(ModuleNode, 0);
				}
				PreviousNode = CurrentNode;
			}
			else
			{
				PreviousNode = nullptr;
			}
		}
	}

	// Reimplementation of GetStackFunctionInputOverridePin (read-only)
	// Mirrors engine logic: checks static switch pins on the FunctionCall node first,
	// then walks upstream to the ParameterMapSet override node for data inputs.
	UEdGraphPin* GetStackFunctionInputOverridePin(UNiagaraNodeFunctionCall& Node, const FNiagaraParameterHandle& AliasedHandle)
	{
		FName HandleName = AliasedHandle.GetParameterHandleString();

		// 1. Check static switch pins on the FunctionCall node itself
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinName == HandleName)
			{
				return Pin;
			}
		}

		// 2. Walk upstream to the ParameterMapSet override node (data inputs live here)
		UEdGraphPin* PMInput = GetParameterMapPin(Node, EGPD_Input);
		if (PMInput && PMInput->LinkedTo.Num() == 1)
		{
			UEdGraphNode* OverrideNode = PMInput->LinkedTo[0]->GetOwningNode();
			if (OverrideNode)
			{
				for (UEdGraphPin* Pin : OverrideNode->Pins)
				{
					if (Pin->Direction == EGPD_Input && Pin->PinName == HandleName)
					{
						return Pin;
					}
				}
			}
		}
		return nullptr;
	}

	// Check if a module is enabled via its metadata
	TOptional<bool> GetModuleIsEnabled(UNiagaraNodeFunctionCall& Node)
	{
		return Node.IsNodeEnabled() ? TOptional<bool>(true) : TOptional<bool>(false);
	}

	// RemoveModuleFromStack — splice the node out of the ParameterMap chain, then destroy it.
	// Uses ParameterMap pins (matching engine's ConnectStackNodeGroup) for correct wiring.
	// Also removes any upstream OverrideNode (UNiagaraNodeParameterMapSet) that was created
	// by GetOrCreateStackFunctionInputOverridePin / set_curve_value for data input overrides.
	bool RemoveModuleFromStack(UNiagaraSystem& System, FGuid EmitterGuid, UNiagaraNodeFunctionCall& ModuleNode)
	{
		UEdGraph* Graph = ModuleNode.GetGraph();
		if (!Graph) return false;

		// Find ParameterMap input/output pins on the target module
		UEdGraphPin* TargetMapIn = GetParameterMapPin(ModuleNode, EGPD_Input);
		UEdGraphPin* TargetMapOut = GetParameterMapPin(ModuleNode, EGPD_Output);

		// Identify upstream and downstream connections
		UEdGraphPin* UpstreamOutputPin = (TargetMapIn && TargetMapIn->LinkedTo.Num() > 0)
			? TargetMapIn->LinkedTo[0] : nullptr;
		UEdGraphPin* DownstreamInputPin = (TargetMapOut && TargetMapOut->LinkedTo.Num() > 0)
			? TargetMapOut->LinkedTo[0] : nullptr;

		// Check if the immediate upstream node is an OverrideNode (ParameterMapSet)
		// that serves this module's data input overrides. If so, we need to walk past it to
		// find the real previous chain node and remove the OverrideNode too.
		// Note: UNiagaraNodeParameterMapSet is only forward-declared, so we work with UEdGraphNode*.
		UEdGraphNode* OverrideNode = nullptr;
		if (UpstreamOutputPin)
		{
			UEdGraphNode* UpstreamNode = UpstreamOutputPin->GetOwningNode();
			// Check if upstream is a ParameterMapSet (not a FunctionCall — those are other modules).
			// IMPORTANT: The stack's input connector (UNiagaraNodeInput) also passes the old check
			// (!FunctionCall && is NiagaraNode) but must NOT be treated as an OverrideNode.
			// Real OverrideNodes (ParameterMapSet) have BOTH ParameterMap input and output pins.
			// The input connector only has an output pin.
			if (UpstreamNode && !Cast<UNiagaraNodeFunctionCall>(UpstreamNode) && Cast<UNiagaraNode>(UpstreamNode))
			{
				UEdGraphPin* CandidateMapIn = GetParameterMapPin(*CastChecked<UNiagaraNode>(UpstreamNode), EGPD_Input);
				UEdGraphPin* CandidateMapOut = GetParameterMapPin(*CastChecked<UNiagaraNode>(UpstreamNode), EGPD_Output);
				if (CandidateMapIn && CandidateMapOut)
				{
					OverrideNode = UpstreamNode;
					// Walk one more step upstream past the override node via its ParameterMap input pin
					if (CandidateMapIn->LinkedTo.Num() > 0)
					{
						UpstreamOutputPin = CandidateMapIn->LinkedTo[0];
					}
				}
			}
		}

		// Splice: reconnect upstream → downstream, bypassing the target (and its OverrideNode)
		if (UpstreamOutputPin && DownstreamInputPin)
		{
			DownstreamInputPin->BreakLinkTo(TargetMapOut);
			TargetMapIn->BreakLinkTo(UpstreamOutputPin);
			DownstreamInputPin->MakeLinkTo(UpstreamOutputPin);
		}

		// Remove the OverrideNode first (if present), then the FunctionCall
		if (OverrideNode)
		{
			OverrideNode->BreakAllNodeLinks();
			Graph->RemoveNode(OverrideNode);
		}

		ModuleNode.BreakAllNodeLinks();
		Graph->RemoveNode(&ModuleNode);
		return true;
	}

	// GetParametersForContext — simplified version that collects known parameters
	void GetParametersForContext(UEdGraph* Graph, UNiagaraSystem& System, TSet<FNiagaraVariableBase>& OutParams)
	{
		// Collect from user store
		FNiagaraUserRedirectionParameterStore& US = System.GetExposedParameters();
		TArrayView<const FNiagaraVariableWithOffset> Vars = US.ReadParameterVariables();
		for (const FNiagaraVariableWithOffset& V : Vars)
		{
			OutParams.Add(V);
		}
	}
	// GetStackFunctionInputs — enumerate input pins and extract real types via schema
	void GetStackFunctionInputs(const UNiagaraNodeFunctionCall& Node, TArray<FNiagaraVariable>& OutInputs)
	{
		OutInputs.Reset();
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin->Direction == EGPD_Input && !Pin->bHidden)
			{
				FNiagaraTypeDefinition TypeDef = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
				FNiagaraVariable Var(TypeDef, Pin->PinName);
				OutInputs.Add(Var);
			}
		}
	}
	// Strip "Module." prefix from engine input names for consistent short names.
	// The engine's GetStackFunctionInputs returns "Module.Gravity" but all our write actions
	// use the short form "Gravity" (CreateAliasedModuleParameterHandle adds the namespace).
	FName StripModulePrefix(FName FullName)
	{
		FString Str = FullName.ToString();
		if (Str.StartsWith(TEXT("Module.")))
		{
			return FName(*Str.Mid(7));
		}
		return FullName;
	}
	// Serialize an FRichCurve to a JSON array of key objects
	TArray<TSharedPtr<FJsonValue>> SerializeCurveKeys(const FRichCurve& Curve)
	{
		TArray<TSharedPtr<FJsonValue>> KeyArr;
		for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
		{
			TSharedRef<FJsonObject> KO = MakeShared<FJsonObject>();
			KO->SetNumberField(TEXT("time"), Key.Time);
			KO->SetNumberField(TEXT("value"), Key.Value);
			KO->SetNumberField(TEXT("arrive_tangent"), Key.ArriveTangent);
			KO->SetNumberField(TEXT("leave_tangent"), Key.LeaveTangent);
			KO->SetStringField(TEXT("interp_mode"),
				Key.InterpMode == RCIM_Constant ? TEXT("constant") :
				Key.InterpMode == RCIM_Linear ? TEXT("linear") : TEXT("cubic"));
			KeyArr.Add(MakeShared<FJsonValueObject>(KO));
		}
		return KeyArr;
	}

	// Serialize a DI's curve data into a JSON object (works for float, color, vec2, vec3, vec4 curves)
	TSharedPtr<FJsonObject> SerializeDICurveData(UNiagaraDataInterface* DI)
	{
		if (!DI) return nullptr;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("di_class"), DI->GetClass()->GetName());

		if (UNiagaraDataInterfaceCurve* FloatCurve = Cast<UNiagaraDataInterfaceCurve>(DI))
		{
			Result->SetField(TEXT("curve"), MakeShared<FJsonValueArray>(SerializeCurveKeys(FloatCurve->Curve)));
		}
		else if (UNiagaraDataInterfaceColorCurve* ColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(DI))
		{
			Result->SetField(TEXT("red"), MakeShared<FJsonValueArray>(SerializeCurveKeys(ColorCurve->RedCurve)));
			Result->SetField(TEXT("green"), MakeShared<FJsonValueArray>(SerializeCurveKeys(ColorCurve->GreenCurve)));
			Result->SetField(TEXT("blue"), MakeShared<FJsonValueArray>(SerializeCurveKeys(ColorCurve->BlueCurve)));
			Result->SetField(TEXT("alpha"), MakeShared<FJsonValueArray>(SerializeCurveKeys(ColorCurve->AlphaCurve)));
		}
		else if (UNiagaraDataInterfaceVector2DCurve* Vec2Curve = Cast<UNiagaraDataInterfaceVector2DCurve>(DI))
		{
			Result->SetField(TEXT("x"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec2Curve->XCurve)));
			Result->SetField(TEXT("y"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec2Curve->YCurve)));
		}
		else if (UNiagaraDataInterfaceVectorCurve* Vec3Curve = Cast<UNiagaraDataInterfaceVectorCurve>(DI))
		{
			Result->SetField(TEXT("x"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec3Curve->XCurve)));
			Result->SetField(TEXT("y"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec3Curve->YCurve)));
			Result->SetField(TEXT("z"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec3Curve->ZCurve)));
		}
		else if (UNiagaraDataInterfaceVector4Curve* Vec4Curve = Cast<UNiagaraDataInterfaceVector4Curve>(DI))
		{
			Result->SetField(TEXT("x"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec4Curve->XCurve)));
			Result->SetField(TEXT("y"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec4Curve->YCurve)));
			Result->SetField(TEXT("z"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec4Curve->ZCurve)));
			Result->SetField(TEXT("w"), MakeShared<FJsonValueArray>(SerializeCurveKeys(Vec4Curve->WCurve)));
		}
		return Result;
	}

	// Parse JSON keys array into an FRichCurve
	void ParseKeysIntoCurve(FRichCurve& Curve, const TArray<TSharedPtr<FJsonValue>>& Keys)
	{
		Curve.Reset();
		TArray<FRichCurveKey> NewKeys;
		NewKeys.Reserve(Keys.Num());
		for (const TSharedPtr<FJsonValue>& KeyVal : Keys)
		{
			const TSharedPtr<FJsonObject>& KO = KeyVal->AsObject();
			if (!KO.IsValid()) continue;
			float Time = static_cast<float>(KO->GetNumberField(TEXT("time")));
			float Value = static_cast<float>(KO->GetNumberField(TEXT("value")));
			float ArriveTangent = KO->HasField(TEXT("arrive_tangent")) ? static_cast<float>(KO->GetNumberField(TEXT("arrive_tangent"))) : 0.f;
			float LeaveTangent = KO->HasField(TEXT("leave_tangent")) ? static_cast<float>(KO->GetNumberField(TEXT("leave_tangent"))) : 0.f;

			ERichCurveInterpMode InterpMode = RCIM_Linear;
			if (KO->HasField(TEXT("interp_mode")))
			{
				FString Mode = KO->GetStringField(TEXT("interp_mode"));
				if (Mode == TEXT("constant")) InterpMode = RCIM_Constant;
				else if (Mode == TEXT("cubic")) InterpMode = RCIM_Cubic;
			}

			NewKeys.Add(FRichCurveKey(Time, Value, ArriveTangent, LeaveTangent, InterpMode));
		}
		Curve.SetKeys(NewKeys);
	}

	// Apply curve keys from config JSON to a DI curve object.
	// Supports config formats:
	//   Float curve:  {"keys": [{time, value}, ...]}
	//   Color curve:  {"red": [{...}], "green": [{...}], "blue": [{...}], "alpha": [{...}]}
	//   Vec2 curve:   {"x": [{...}], "y": [{...}]}
	//   Vec3 curve:   {"x": [{...}], "y": [{...}], "z": [{...}]}
	//   Vec4 curve:   {"x": [{...}], "y": [{...}], "z": [{...}], "w": [{...}]}
	bool ApplyCurveConfig(UNiagaraDataInterface* DI, const TSharedPtr<FJsonObject>& Config)
	{
		if (!DI || !Config.IsValid()) return false;

		auto GetKeysArray = [&Config](const FString& FieldName) -> TArray<TSharedPtr<FJsonValue>>
		{
			TSharedPtr<FJsonValue> Field = Config->TryGetField(FieldName);
			if (!Field.IsValid()) return {};
			if (Field->Type == EJson::Array) return Field->AsArray();
			if (Field->Type == EJson::String)
			{
				TArray<TSharedPtr<FJsonValue>> Parsed;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Field->AsString());
				FJsonSerializer::Deserialize(Reader, Parsed);
				return Parsed;
			}
			return {};
		};

		if (UNiagaraDataInterfaceCurve* FloatCurve = Cast<UNiagaraDataInterfaceCurve>(DI))
		{
			TArray<TSharedPtr<FJsonValue>> Keys = GetKeysArray(TEXT("keys"));
			if (Keys.Num() == 0) Keys = GetKeysArray(TEXT("curve"));
			if (Keys.Num() > 0) { ParseKeysIntoCurve(FloatCurve->Curve, Keys); return true; }
		}
		else if (UNiagaraDataInterfaceColorCurve* ColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(DI))
		{
			TArray<TSharedPtr<FJsonValue>> R = GetKeysArray(TEXT("red"));
			TArray<TSharedPtr<FJsonValue>> G = GetKeysArray(TEXT("green"));
			TArray<TSharedPtr<FJsonValue>> B = GetKeysArray(TEXT("blue"));
			TArray<TSharedPtr<FJsonValue>> A = GetKeysArray(TEXT("alpha"));
			bool bAny = R.Num() > 0 || G.Num() > 0 || B.Num() > 0 || A.Num() > 0;
			if (bAny)
			{
				// Full replace: specified channels get new keys, unspecified channels are cleared
				if (R.Num() > 0) ParseKeysIntoCurve(ColorCurve->RedCurve, R); else ColorCurve->RedCurve.Reset();
				if (G.Num() > 0) ParseKeysIntoCurve(ColorCurve->GreenCurve, G); else ColorCurve->GreenCurve.Reset();
				if (B.Num() > 0) ParseKeysIntoCurve(ColorCurve->BlueCurve, B); else ColorCurve->BlueCurve.Reset();
				if (A.Num() > 0) ParseKeysIntoCurve(ColorCurve->AlphaCurve, A); else ColorCurve->AlphaCurve.Reset();
			}
			return bAny;
		}
		else if (UNiagaraDataInterfaceVector2DCurve* Vec2Curve = Cast<UNiagaraDataInterfaceVector2DCurve>(DI))
		{
			TArray<TSharedPtr<FJsonValue>> X = GetKeysArray(TEXT("x"));
			TArray<TSharedPtr<FJsonValue>> Y = GetKeysArray(TEXT("y"));
			bool bAny = X.Num() > 0 || Y.Num() > 0;
			if (bAny)
			{
				if (X.Num() > 0) ParseKeysIntoCurve(Vec2Curve->XCurve, X); else Vec2Curve->XCurve.Reset();
				if (Y.Num() > 0) ParseKeysIntoCurve(Vec2Curve->YCurve, Y); else Vec2Curve->YCurve.Reset();
			}
			return bAny;
		}
		else if (UNiagaraDataInterfaceVectorCurve* Vec3Curve = Cast<UNiagaraDataInterfaceVectorCurve>(DI))
		{
			TArray<TSharedPtr<FJsonValue>> X = GetKeysArray(TEXT("x"));
			TArray<TSharedPtr<FJsonValue>> Y = GetKeysArray(TEXT("y"));
			TArray<TSharedPtr<FJsonValue>> Z = GetKeysArray(TEXT("z"));
			bool bAny = X.Num() > 0 || Y.Num() > 0 || Z.Num() > 0;
			if (bAny)
			{
				if (X.Num() > 0) ParseKeysIntoCurve(Vec3Curve->XCurve, X); else Vec3Curve->XCurve.Reset();
				if (Y.Num() > 0) ParseKeysIntoCurve(Vec3Curve->YCurve, Y); else Vec3Curve->YCurve.Reset();
				if (Z.Num() > 0) ParseKeysIntoCurve(Vec3Curve->ZCurve, Z); else Vec3Curve->ZCurve.Reset();
			}
			return bAny;
		}
		else if (UNiagaraDataInterfaceVector4Curve* Vec4Curve = Cast<UNiagaraDataInterfaceVector4Curve>(DI))
		{
			TArray<TSharedPtr<FJsonValue>> X = GetKeysArray(TEXT("x"));
			TArray<TSharedPtr<FJsonValue>> Y = GetKeysArray(TEXT("y"));
			TArray<TSharedPtr<FJsonValue>> Z = GetKeysArray(TEXT("z"));
			TArray<TSharedPtr<FJsonValue>> W = GetKeysArray(TEXT("w"));
			bool bAny = X.Num() > 0 || Y.Num() > 0 || Z.Num() > 0 || W.Num() > 0;
			if (bAny)
			{
				if (X.Num() > 0) ParseKeysIntoCurve(Vec4Curve->XCurve, X); else Vec4Curve->XCurve.Reset();
				if (Y.Num() > 0) ParseKeysIntoCurve(Vec4Curve->YCurve, Y); else Vec4Curve->YCurve.Reset();
				if (Z.Num() > 0) ParseKeysIntoCurve(Vec4Curve->ZCurve, Z); else Vec4Curve->ZCurve.Reset();
				if (W.Num() > 0) ParseKeysIntoCurve(Vec4Curve->WCurve, W); else Vec4Curve->WCurve.Reset();
			}
			return bAny;
		}
		return false;
	}

} // namespace MonolithNiagaraHelpers

// Helper: wrap a string result in a FJsonObject for FMonolithActionResult::Success
static FMonolithActionResult SuccessStr(const FString& Msg)
{
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("result"), Msg);
	return FMonolithActionResult::Success(R);
}

// Helper: wrap a pre-built JSON object for Success
static FMonolithActionResult SuccessObj(const TSharedRef<FJsonObject>& Obj)
{
	return FMonolithActionResult::Success(Obj);
}

// Helper: normalize asset path parameter — accepts "asset_path" (preferred) with "system_path" fallback
static FString GetAssetPath(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = Params->GetStringField(TEXT("asset_path"));
	if (Path.IsEmpty()) Path = Params->GetStringField(TEXT("system_path"));
	return Path;
}

// ============================================================================
// JSON Helpers
// ============================================================================

FString FMonolithNiagaraActions::JsonObjectToString(const TSharedRef<FJsonObject>& JsonObj)
{
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(JsonObj, W);
	return Out;
}

FString FMonolithNiagaraActions::JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& JsonArray)
{
	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(JsonArray, W);
	return Out;
}

FString FMonolithNiagaraActions::JsonValueToString(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return FString();
	if (Value->Type == EJson::String) return FString::Printf(TEXT("\"%s\""), *Value->AsString());
	if (Value->Type == EJson::Number) return FString::SanitizeFloat(Value->AsNumber());
	if (Value->Type == EJson::Boolean) return Value->AsBool() ? TEXT("true") : TEXT("false");
	if (Value->Type == EJson::Object)
	{
		FString R;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&R);
		FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), W);
		return R;
	}
	if (Value->Type == EJson::Array)
	{
		FString R;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&R);
		FJsonSerializer::Serialize(Value->AsArray(), W);
		return R;
	}
	return FString();
}

// ============================================================================
// Core Helpers
// ============================================================================

UNiagaraSystem* FMonolithNiagaraActions::LoadSystem(const FString& SystemPath)
{
	UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(SystemPath);
	if (!System)
	{
		UE_LOG(LogMonolithNiagara, Error, TEXT("Failed to load Niagara system: %s"), *SystemPath);
	}
	return System;
}

int32 FMonolithNiagaraActions::FindEmitterHandleIndex(UNiagaraSystem* System, const FString& HandleIdOrName)
{
	if (!System || HandleIdOrName.IsEmpty()) return INDEX_NONE;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	// Try GUID match first
	FGuid TestGuid;
	if (FGuid::Parse(HandleIdOrName, TestGuid))
	{
		for (int32 i = 0; i < Handles.Num(); ++i)
		{
			if (Handles[i].GetId() == TestGuid) return i;
		}
	}

	// Exact FName match
	FName TestName(*HandleIdOrName);
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetName() == TestName) return i;
	}

	// Case-insensitive name match
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetName().ToString().Equals(HandleIdOrName, ESearchCase::IgnoreCase)) return i;
	}

	// Unique instance name match (can differ from handle display name)
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (Handles[i].GetUniqueInstanceName().Equals(HandleIdOrName, ESearchCase::IgnoreCase)) return i;
	}

	// Numeric index fallback ("0", "1", "2")
	if (HandleIdOrName.IsNumeric())
	{
		int32 Idx = FCString::Atoi(*HandleIdOrName);
		if (Handles.IsValidIndex(Idx)) return Idx;
	}

	// If only one emitter exists and caller passed empty string, auto-select it.
	// Do NOT auto-select when a specific name was given that didn't match — that's a bug.
	if (Handles.Num() == 1 && HandleIdOrName.IsEmpty()) return 0;

	return INDEX_NONE;
}

bool FMonolithNiagaraActions::ResolveScriptUsage(const FString& UsageString, ENiagaraScriptUsage& OutUsage)
{
	FString L = UsageString.ToLower();
	if (L == TEXT("system_spawn") || L == TEXT("systemspawn")) { OutUsage = ENiagaraScriptUsage::SystemSpawnScript; return true; }
	if (L == TEXT("system_update") || L == TEXT("systemupdate")) { OutUsage = ENiagaraScriptUsage::SystemUpdateScript; return true; }
	if (L == TEXT("emitter_spawn") || L == TEXT("emitterspawn")) { OutUsage = ENiagaraScriptUsage::EmitterSpawnScript; return true; }
	if (L == TEXT("emitter_update") || L == TEXT("emitterupdate")) { OutUsage = ENiagaraScriptUsage::EmitterUpdateScript; return true; }
	if (L == TEXT("particle_spawn") || L == TEXT("particlespawn") || L == TEXT("spawn")) { OutUsage = ENiagaraScriptUsage::ParticleSpawnScript; return true; }
	if (L == TEXT("particle_update") || L == TEXT("particleupdate") || L == TEXT("update")) { OutUsage = ENiagaraScriptUsage::ParticleUpdateScript; return true; }
	return false;
}

FString FMonolithNiagaraActions::UsageToString(ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("system_spawn");
	case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("system_update");
	case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("emitter_spawn");
	case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("emitter_update");
	case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("particle_spawn");
	case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("particle_update");
	default: return TEXT("unknown");
	}
}

UNiagaraGraph* FMonolithNiagaraActions::GetGraphForUsage(UNiagaraSystem* System, const FString& EmitterHandleId, ENiagaraScriptUsage Usage)
{
	if (!System) return nullptr;

	if (Usage == ENiagaraScriptUsage::SystemSpawnScript || Usage == ENiagaraScriptUsage::SystemUpdateScript)
	{
		// System spawn and update share a single graph — accessed via the system spawn script
		UNiagaraScript* Script = System->GetSystemSpawnScript();
		if (!Script) return nullptr;
		UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		return Src ? Src->NodeGraph : nullptr;
	}
	else
	{
		// Emitter scripts (emitter spawn/update, particle spawn/update) share a single graph
		// accessed via ED->GraphSource — NOT via individual GetScript() calls
		int32 Idx = FindEmitterHandleIndex(System, EmitterHandleId);
		if (Idx == INDEX_NONE) return nullptr;
		FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[Idx].GetEmitterData();
		if (!ED) return nullptr;
		UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(ED->GraphSource);
		return Src ? Src->NodeGraph : nullptr;
	}
}

UNiagaraNodeOutput* FMonolithNiagaraActions::FindOutputNode(UNiagaraSystem* System, const FString& EmitterHandleId, ENiagaraScriptUsage Usage)
{
	UNiagaraGraph* Graph = GetGraphForUsage(System, EmitterHandleId, Usage);
	if (!Graph) return nullptr;
	return Graph->FindEquivalentOutputNode(Usage, FGuid());
}

UNiagaraNodeFunctionCall* FMonolithNiagaraActions::FindModuleNode(UNiagaraSystem* System, const FString& EmitterHandleId,
	const FString& NodeGuidStr, ENiagaraScriptUsage* OutUsage)
{
	FGuid TargetGuid;
	bool bHasGuid = FGuid::Parse(NodeGuidStr, TargetGuid);

	static const ENiagaraScriptUsage AllUsages[] = {
		ENiagaraScriptUsage::SystemSpawnScript, ENiagaraScriptUsage::SystemUpdateScript,
		ENiagaraScriptUsage::EmitterSpawnScript, ENiagaraScriptUsage::EmitterUpdateScript,
		ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript,
	};

	// Pass 1: Walk the ParameterMap chain from each output node (fast path, connected graphs)
	for (ENiagaraScriptUsage Usage : AllUsages)
	{
		UNiagaraNodeOutput* Out = FindOutputNode(System, EmitterHandleId, Usage);
		if (!Out) continue;
		TArray<UNiagaraNodeFunctionCall*> Mods;
		MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
		for (UNiagaraNodeFunctionCall* N : Mods)
		{
			if (!N) continue;
			if ((bHasGuid && N->NodeGuid == TargetGuid) || N->GetFunctionName() == NodeGuidStr)
			{
				if (OutUsage) *OutUsage = Usage;
				return N;
			}
		}
	}

	// Pass 2: Fallback — scan all UNiagaraNodeFunctionCall nodes directly in each graph.
	// Handles broken/disconnected ParameterMap chains where the chain traversal returns nothing
	// but the module nodes still exist as orphaned objects in the graph (e.g. after a corrupt
	// create_system_from_spec that added modules without wiring the chain).
	TArray<UNiagaraGraph*> VisitedGraphs;
	for (ENiagaraScriptUsage Usage : AllUsages)
	{
		UNiagaraGraph* Graph = GetGraphForUsage(System, EmitterHandleId, Usage);
		if (!Graph || VisitedGraphs.Contains(Graph)) continue;
		VisitedGraphs.Add(Graph);

		TArray<UNiagaraNodeFunctionCall*> AllFunctionCalls;
		Graph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllFunctionCalls);
		for (UNiagaraNodeFunctionCall* N : AllFunctionCalls)
		{
			if (!N) continue;
			if ((bHasGuid && N->NodeGuid == TargetGuid) || N->GetFunctionName() == NodeGuidStr)
			{
				// Best-effort usage: check which output node this node's script usage matches
				if (OutUsage) *OutUsage = Usage;
				return N;
			}
		}
	}

	// Pass 3: If no graphs were visited (emitter resolution failed for all usages), scan ALL
	// emitter handles as a last resort. This catches cases where the emitter identifier is wrong
	// but the module GUID is valid somewhere in the system.
	if (VisitedGraphs.Num() == 0)
	{
		for (int32 i = 0; i < System->GetEmitterHandles().Num(); i++)
		{
			const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[i];
			const FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			if (!ED) continue;
			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);
			if (!Source) continue;
			UNiagaraGraph* Graph = Source->NodeGraph;
			if (!Graph || VisitedGraphs.Contains(Graph)) continue;
			VisitedGraphs.Add(Graph);

			for (UEdGraphNode* N : Graph->Nodes)
			{
				UNiagaraNodeFunctionCall* FN = Cast<UNiagaraNodeFunctionCall>(N);
				if (!FN) continue;
				if (bHasGuid && FN->NodeGuid == TargetGuid)
				{
					if (OutUsage) *OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
					return FN;
				}
				if (!bHasGuid && FN->GetFunctionName() == NodeGuidStr)
				{
					if (OutUsage) *OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
					return FN;
				}
			}
		}
	}

	return nullptr;
}

UClass* FMonolithNiagaraActions::ResolveRendererClass(const FString& RendererClass)
{
	FString L = RendererClass.ToLower();
	if (L == TEXT("sprite") || L == TEXT("spriterenderer")) return UNiagaraSpriteRendererProperties::StaticClass();
	if (L == TEXT("mesh") || L == TEXT("meshrenderer")) return UNiagaraMeshRendererProperties::StaticClass();
	if (L == TEXT("ribbon") || L == TEXT("ribbonrenderer")) return UNiagaraRibbonRendererProperties::StaticClass();
	if (L == TEXT("light") || L == TEXT("lightrenderer")) return UNiagaraLightRendererProperties::StaticClass();
	if (L == TEXT("component") || L == TEXT("componentrenderer")) return UNiagaraComponentRendererProperties::StaticClass();

	// Normalize to bare segment (e.g. "Sprite", "Mesh"), then rebuild full class name.
	// Handles all input forms: "Sprite", "SpriteRenderer", "NiagaraSpriteRendererProperties",
	// "UNiagaraSpriteRendererProperties" — all produce "UNiagaraSpriteRendererProperties".
	FString Seg = RendererClass;
	if (Seg.StartsWith(TEXT("UNiagara"))) Seg = Seg.Mid(8);
	else if (Seg.StartsWith(TEXT("Niagara"))) Seg = Seg.Mid(7);
	if (Seg.EndsWith(TEXT("RendererProperties"))) Seg = Seg.LeftChop(18);
	else if (Seg.EndsWith(TEXT("Renderer"))) Seg = Seg.LeftChop(8);
	FString Full = TEXT("UNiagara") + Seg + TEXT("RendererProperties");
	UClass* C = FindFirstObject<UClass>(*Full, EFindFirstObjectOptions::NativeFirst);
	if (!C) C = FindFirstObject<UClass>(*Full.Mid(1), EFindFirstObjectOptions::NativeFirst);
	// Never return the abstract base class — instantiating it triggers a pure-virtual crash in CreateBoundsCalculator
	if (C == UNiagaraRendererProperties::StaticClass()) return nullptr;
	return C;
}

UNiagaraRendererProperties* FMonolithNiagaraActions::GetRenderer(UNiagaraSystem* System, const FString& EmitterHandleId,
	int32 RendererIndex, FVersionedNiagaraEmitterData** OutEmitterData)
{
	if (!System) return nullptr;
	int32 Idx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Idx == INDEX_NONE) return nullptr;
	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[Idx].GetEmitterData();
	if (!ED) return nullptr;
	if (OutEmitterData) *OutEmitterData = ED;
	const TArray<UNiagaraRendererProperties*>& R = ED->GetRenderers();
	return R.IsValidIndex(RendererIndex) ? R[RendererIndex] : nullptr;
}

FNiagaraTypeDefinition FMonolithNiagaraActions::ResolveNiagaraType(const FString& TypeName)
{
	FString L = TypeName.ToLower();
	if (L == TEXT("float")) return FNiagaraTypeDefinition::GetFloatDef();
	if (L == TEXT("int") || L == TEXT("int32")) return FNiagaraTypeDefinition::GetIntDef();
	if (L == TEXT("bool")) return FNiagaraTypeDefinition::GetBoolDef();
	if (L == TEXT("vec2") || L == TEXT("vector2d") || L == TEXT("vector2")) return FNiagaraTypeDefinition::GetVec2Def();
	if (L == TEXT("vec3") || L == TEXT("vector") || L == TEXT("vector3")) return FNiagaraTypeDefinition::GetVec3Def();
	if (L == TEXT("vec4") || L == TEXT("vector4")) return FNiagaraTypeDefinition::GetVec4Def();
	if (L == TEXT("color") || L == TEXT("linearcolor")) return FNiagaraTypeDefinition::GetColorDef();
	if (L == TEXT("position")) return FNiagaraTypeDefinition::GetPositionDef();
	if (L == TEXT("quat") || L == TEXT("quaternion")) return FNiagaraTypeDefinition::GetQuatDef();
	if (L == TEXT("matrix") || L == TEXT("matrix4")) return FNiagaraTypeDefinition::GetMatrix4Def();
	return FNiagaraTypeDefinition::GetFloatDef();
}

FString FMonolithNiagaraActions::SerializeParameterValue(const FNiagaraVariable& Variable, const FNiagaraParameterStore& Store)
{
	const FNiagaraTypeDefinition& T = Variable.GetType();
	if (T == FNiagaraTypeDefinition::GetFloatDef()) return FString::SanitizeFloat(Store.GetParameterValue<float>(Variable));
	if (T == FNiagaraTypeDefinition::GetIntDef()) return FString::FromInt(Store.GetParameterValue<int32>(Variable));
	if (T == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool V = Store.GetParameterValue<FNiagaraBool>(Variable);
		return V.IsValid() && V.GetValue() ? TEXT("true") : TEXT("false");
	}
	if (T == FNiagaraTypeDefinition::GetVec2Def())
	{
		FVector2f V = Store.GetParameterValue<FVector2f>(Variable);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y);
		return JsonObjectToString(O);
	}
	if (T == FNiagaraTypeDefinition::GetVec3Def() || T == FNiagaraTypeDefinition::GetPositionDef())
	{
		FVector3f V = Store.GetParameterValue<FVector3f>(Variable);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return JsonObjectToString(O);
	}
	if (T == FNiagaraTypeDefinition::GetVec4Def() || T == FNiagaraTypeDefinition::GetQuatDef())
	{
		FVector4f V = Store.GetParameterValue<FVector4f>(Variable);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z); O->SetNumberField(TEXT("w"), V.W);
		return JsonObjectToString(O);
	}
	if (T == FNiagaraTypeDefinition::GetColorDef())
	{
		FLinearColor V = Store.GetParameterValue<FLinearColor>(Variable);
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("r"), V.R); O->SetNumberField(TEXT("g"), V.G);
		O->SetNumberField(TEXT("b"), V.B); O->SetNumberField(TEXT("a"), V.A);
		return JsonObjectToString(O);
	}
	return TEXT("\"<unsupported>\"");
}

FNiagaraVariable FMonolithNiagaraActions::MakeUserVariable(const FString& ParamName, const FNiagaraTypeDefinition& TypeDef)
{
	FString Full = ParamName;
	if (!Full.StartsWith(TEXT("User."))) Full = TEXT("User.") + Full;
	return FNiagaraVariable(TypeDef, FName(*Full));
}

// Helper: extract JSON object from a value that may be an object or a string-serialized object
// (Claude Code often serializes nested JSON objects as strings via MCP)
static TSharedPtr<FJsonObject> AsObjectOrParseString(const TSharedPtr<FJsonValue>& JsonValue)
{
	if (!JsonValue.IsValid()) return nullptr;
	TSharedPtr<FJsonObject> O = JsonValue->AsObject();
	// UE 5.7: AsObject() on a FJsonValueString returns a valid but EMPTY FJsonObject
	// instead of nullptr. Check that the object actually has fields before accepting it.
	if (O.IsValid() && O->Values.Num() > 0) return O;
	// Fallback: try parsing as a JSON string
	// Claude Code double-serializes MCP params, so strings may contain backslash-escaped quotes
	// e.g. {\"r\":1,\"g\":0} instead of {"r":1,"g":0}. Unescape before parsing.
	if (JsonValue->Type == EJson::String)
	{
		FString Str = JsonValue->AsString();
		Str.ReplaceInline(TEXT("\\\""), TEXT("\""), ESearchCase::CaseSensitive);
		Str.ReplaceInline(TEXT("\\\\"), TEXT("\\"), ESearchCase::CaseSensitive);
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
		FJsonSerializer::Deserialize(Reader, O);
	}
	return O;
}

// Helper: set typed value on parameter store from JSON
static bool SetTypedParameterValue(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Var,
	const FNiagaraTypeDefinition& TypeDef, const TSharedPtr<FJsonValue>& JsonValue)
{
	if (!JsonValue.IsValid()) return false;
	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		Store.SetParameterValue<float>(static_cast<float>(JsonValue->AsNumber()), Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		Store.SetParameterValue<int32>(static_cast<int32>(JsonValue->AsNumber()), Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool V; V.SetValue(JsonValue->AsBool());
		Store.SetParameterValue<FNiagaraBool>(V, Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(JsonValue);
		if (!O) return false;
		FVector2f V(static_cast<float>(O->GetNumberField(TEXT("x"))), static_cast<float>(O->GetNumberField(TEXT("y"))));
		Store.SetParameterValue<FVector2f>(V, Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetVec3Def() || TypeDef == FNiagaraTypeDefinition::GetPositionDef())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(JsonValue);
		if (!O) return false;
		FVector3f V(static_cast<float>(O->GetNumberField(TEXT("x"))), static_cast<float>(O->GetNumberField(TEXT("y"))),
			static_cast<float>(O->GetNumberField(TEXT("z"))));
		Store.SetParameterValue<FVector3f>(V, Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetVec4Def() || TypeDef == FNiagaraTypeDefinition::GetQuatDef())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(JsonValue);
		if (!O) return false;
		FVector4f V(static_cast<float>(O->GetNumberField(TEXT("x"))), static_cast<float>(O->GetNumberField(TEXT("y"))),
			static_cast<float>(O->GetNumberField(TEXT("z"))), static_cast<float>(O->GetNumberField(TEXT("w"))));
		Store.SetParameterValue<FVector4f>(V, Var, true);
		return true;
	}
	if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(JsonValue);
		if (!O) return false;
		FLinearColor V(static_cast<float>(O->GetNumberField(TEXT("r"))), static_cast<float>(O->GetNumberField(TEXT("g"))),
			static_cast<float>(O->GetNumberField(TEXT("b"))),
			O->HasField(TEXT("a")) ? static_cast<float>(O->GetNumberField(TEXT("a"))) : 1.0f);
		Store.SetParameterValue<FLinearColor>(V, Var, true);
		return true;
	}
	return false;
}

// Helper: collect params from a store
static void CollectParametersFromStore(const FNiagaraParameterStore& Store, const FString& Scope,
	TArray<TSharedPtr<FJsonValue>>& OutArray)
{
	TArrayView<const FNiagaraVariableWithOffset> Variables = Store.ReadParameterVariables();
	for (const FNiagaraVariableWithOffset& VWO : Variables)
	{
		const FNiagaraVariable& Var = VWO;
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Var.GetName().ToString());
		P->SetStringField(TEXT("type"), Var.GetType().GetName());
		P->SetStringField(TEXT("scope"), Scope);
		P->SetStringField(TEXT("value"), FMonolithNiagaraActions::SerializeParameterValue(Var, Store));
		OutArray.Add(MakeShared<FJsonValueObject>(P));
	}
}

// ============================================================================
// Registration — 39 actions across 7 domains
// ============================================================================

void FMonolithNiagaraActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	// System (8)
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_emitter"), TEXT("Add an emitter to a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleAddEmitter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter_asset"), TEXT("string"), TEXT("Emitter asset path to add"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Custom name for the emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_emitter"), TEXT("Remove an emitter from a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEmitter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("duplicate_emitter"), TEXT("Duplicate an emitter within a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateEmitter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("source_emitter"), TEXT("string"), TEXT("Name of emitter to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Name for the duplicated emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_enabled"), TEXT("Enable or disable an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterEnabled),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Whether to enable the emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("reorder_emitters"), TEXT("Reorder emitters in a system"),
		FMonolithActionHandler::CreateStatic(&HandleReorderEmitters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("order"), TEXT("array"), TEXT("Array of emitter names in desired order"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_property"), TEXT("Set an emitter property"),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("request_compile"), TEXT("Request compilation of a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleRequestCompile),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_system"), TEXT("Create a new Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSystem),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Path to save the new system"))
			.Optional(TEXT("template"), TEXT("string"), TEXT("Template system to base on"))
			.Build());

	// Module (12)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_ordered_modules"), TEXT("Get ordered modules in a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleGetOrderedModules),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Script usage filter (e.g. Spawn, Update, Event)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_inputs"), TEXT("Get inputs for a module node"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleInputs),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_graph"), TEXT("Get the node graph of a module script"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleGraph),
		FParamSchemaBuilder()
			.Required(TEXT("script_path"), TEXT("string"), TEXT("Module script asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_module"), TEXT("Add a module to a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleAddModule),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("usage"), TEXT("string"), TEXT("Script usage (Spawn, Update, Event)"))
			.Required(TEXT("module_script"), TEXT("string"), TEXT("Module script asset path"))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Position to insert the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_module"), TEXT("Remove a module from a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveModule),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("move_module"), TEXT("Move a module to a new index"),
		FMonolithActionHandler::CreateStatic(&HandleMoveModule),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("new_index"), TEXT("integer"), TEXT("New position index"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_enabled"), TEXT("Enable or disable a module"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleEnabled),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Whether to enable the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_value"), TEXT("Set a module input value"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_binding"), TEXT("Bind a module input to a parameter"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("binding"), TEXT("string"), TEXT("Parameter binding path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_di"), TEXT("Set a data interface on a module input"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputDI),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("di_class"), TEXT("string"), TEXT("Data interface class name"))
			.Optional(TEXT("config"), TEXT("object"), TEXT("Data interface configuration"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_module_from_hlsl"), TEXT("Create a Niagara module script from custom HLSL"),
		FMonolithActionHandler::CreateStatic(&HandleCreateModuleFromHLSL),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Display name for the module"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path to save (e.g. /Game/VFX/Modules/MyModule)"))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("HLSL code body"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type} objects for input parameters"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for output parameters"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description for the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_function_from_hlsl"), TEXT("Create a Niagara function script from custom HLSL"),
		FMonolithActionHandler::CreateStatic(&HandleCreateFunctionFromHLSL),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Display name for the function"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset path to save (e.g. /Game/VFX/Functions/MyFunc)"))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("HLSL code body"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type} objects for input parameters"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for output parameters"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description for the function"))
			.Build());

	// Parameter (9)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_all_parameters"), TEXT("Get all parameters in a system"),
		FMonolithActionHandler::CreateStatic(&HandleGetAllParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Filter to a specific emitter by name"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Filter by scope (e.g. 'User', 'ParticleSpawn', emitter name)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_user_parameters"), TEXT("Get user-exposed parameters"),
		FMonolithActionHandler::CreateStatic(&HandleGetUserParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_parameter_value"), TEXT("Get a parameter value"),
		FMonolithActionHandler::CreateStatic(&HandleGetParameterValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("parameter"), TEXT("string"), TEXT("Parameter name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_parameter_type"), TEXT("Get info about a Niagara type"),
		FMonolithActionHandler::CreateStatic(&HandleGetParameterType),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Niagara type name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("trace_parameter_binding"), TEXT("Trace where a parameter is used"),
		FMonolithActionHandler::CreateStatic(&HandleTraceParameterBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("parameter"), TEXT("string"), TEXT("Parameter name to trace"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_user_parameter"), TEXT("Add a user parameter"),
		FMonolithActionHandler::CreateStatic(&HandleAddUserParameter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("type"), TEXT("string"), TEXT("Niagara type name"))
			.Optional(TEXT("default"), TEXT("string"), TEXT("Default value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_user_parameter"), TEXT("Remove a user parameter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveUserParameter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_parameter_default"), TEXT("Set a parameter default value"),
		FMonolithActionHandler::CreateStatic(&HandleSetParameterDefault),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("parameter"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Default value to set"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_curve_value"), TEXT("Set curve keys on a module input"),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("Array of curve key objects"))
			.Build());

	// Renderer (6)
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_renderer"), TEXT("Add a renderer to an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleAddRenderer),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("class"), TEXT("string"), TEXT("Renderer class (e.g. Sprite, Mesh, Ribbon)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_renderer"), TEXT("Remove a renderer from an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveRenderer),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Index of the renderer to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_material"), TEXT("Set renderer material"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("material"), TEXT("string"), TEXT("Material asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_property"), TEXT("Set a renderer property"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_renderer_bindings"), TEXT("Get renderer attribute bindings"),
		FMonolithActionHandler::CreateStatic(&HandleGetRendererBindings),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_binding"), TEXT("Set a renderer attribute binding"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererBinding),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("binding_name"), TEXT("string"), TEXT("Binding property name"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Particle attribute to bind"))
			.Build());

	// Read (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_emitters"), TEXT("List all emitters in a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleListEmitters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_renderers"), TEXT("List all renderers on a specific emitter"),
		FMonolithActionHandler::CreateStatic(&HandleListRenderers),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());

	// Discovery (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_module_scripts"), TEXT("Search available Niagara module scripts by keyword"),
		FMonolithActionHandler::CreateStatic(&HandleListModuleScripts),
		FParamSchemaBuilder()
			.Optional(TEXT("search"), TEXT("string"), TEXT("Search keyword (e.g. 'gravity', 'color', 'velocity'). Omit to list all."))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Filter by usage: 'module', 'dynamic_input', 'function'. Default: module."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results (default: 50)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_renderer_properties"), TEXT("List editable properties on a renderer"),
		FMonolithActionHandler::CreateStatic(&HandleListRendererProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index (from list_renderers)"))
			.Build());

	// Batch (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("batch_execute"), TEXT("Execute multiple operations in one transaction"),
		FMonolithActionHandler::CreateStatic(&HandleBatchExecute),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("operations"), TEXT("array"), TEXT("Array of operation objects to execute"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_system_from_spec"), TEXT("Create a full system from JSON spec"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSystemFromSpec),
		FParamSchemaBuilder()
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Asset save path, e.g. /Game/VFX/NS_MySystem"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("System specification (emitters, modules, renderers, user_parameters)"))
			.Build());

	// DI (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_di_functions"), TEXT("Get data interface function signatures"),
		FMonolithActionHandler::CreateStatic(&HandleGetDIFunctions),
		FParamSchemaBuilder()
			.Required(TEXT("di_class"), TEXT("string"), TEXT("Data interface class name"))
			.Build());

	// HLSL (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_compiled_gpu_hlsl"), TEXT("Get compiled GPU HLSL for an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompiledGPUHLSL),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());

	// Diagnostics (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_diagnostics"), TEXT("Get compile errors, warnings, renderer issues, and script stats"),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemDiagnostics),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Optional(TEXT("compile_first"), TEXT("boolean"), TEXT("Force synchronous compile before collecting diagnostics (default: true)"))
			.Build());

	// System Property (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_property"), TEXT("Read a system-level property (WarmupTime, bDeterminism, RandomSeed, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name or snake_case alias: warmup_time, determinism, random_seed, max_pool_size, etc."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_system_property"), TEXT("Set a system-level property (WarmupTime, bDeterminism, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetSystemProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property: WarmupTime, WarmupTickCount, WarmupTickDelta, bFixedTickDelta, FixedTickDeltaTime, bDeterminism, RandomSeed, bSupportLargeWorldCoordinates, bNeedsSortedSignificanceHandling, SignificanceHandlerLink, MaxPoolSize"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());

	// Static Switch (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_static_switch_value"), TEXT("Set a static switch value on a module"),
		FMonolithActionHandler::CreateStatic(&HandleSetStaticSwitchValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Static switch input name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set (true/false for bool, enum value name for enums, integer for int switches)"))
			.Build());

	// --- Wave 2: Summary & Discovery (4 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_summary"), TEXT("One-call overview of an entire Niagara system (emitters, params, renderers, module counts)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_summary"), TEXT("Deep view of a single emitter (modules per stage, renderers, properties)"),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterSummary),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or GUID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_emitter_properties"), TEXT("List all editable properties on FVersionedNiagaraEmitterData with current values"),
		FMonolithActionHandler::CreateStatic(&HandleListEmitterProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_input_value"), TEXT("Read the current override value for a specific module input"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleInputValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name (bare or Module. prefixed)"))
			.Build());

	// --- Wave 3: DI Curve & Configuration (2 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_curve_keys"), TEXT("Set keys on an existing curve DI attached to a module input"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureCurveKeys),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name that has a curve DI"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("For float: [{time,value}]; color: [{time,r,g,b,a}]; vector: [{time,x,y,z}]"))
			.Optional(TEXT("interp"), TEXT("string"), TEXT("Interpolation: linear, cubic, constant (default: cubic)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_data_interface"), TEXT("Set arbitrary properties on a DI attached to a module input via reflection"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureDataInterface),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Property name-value pairs to set on the DI"))
			.Build());

	// --- Wave 4: System Management (5 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("duplicate_system"), TEXT("Clone an entire Niagara system to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateSystem),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Source system asset path"))
			.Required(TEXT("save_path"), TEXT("string"), TEXT("Destination path (e.g. /Game/VFX/NS_Fire_Copy)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_fixed_bounds"), TEXT("Set explicit bounds on system or emitter for GPU performance"),
		FMonolithActionHandler::CreateStatic(&HandleSetFixedBounds),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Emitter name (omit for system-level bounds)"))
			.Required(TEXT("min"), TEXT("array"), TEXT("Min bounds [x, y, z]"))
			.Required(TEXT("max"), TEXT("array"), TEXT("Max bounds [x, y, z]"))
			.Optional(TEXT("enabled"), TEXT("bool"), TEXT("Enable fixed bounds (default: true). Set false to re-enable dynamic."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_effect_type"), TEXT("Assign a UNiagaraEffectType for scalability and cull distance"),
		FMonolithActionHandler::CreateStatic(&HandleSetEffectType),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("effect_type"), TEXT("string"), TEXT("Effect type asset path, or 'none' to clear"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_emitter"), TEXT("Add a minimal empty emitter to a system (no template needed)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateEmitter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Emitter name"))
			.Optional(TEXT("sim_target"), TEXT("string"), TEXT("cpu or gpu (default: cpu)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("export_system_spec"), TEXT("Reverse-engineer an existing system into create_system_from_spec-compatible JSON"),
		FMonolithActionHandler::CreateStatic(&HandleExportSystemSpec),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Optional(TEXT("include_values"), TEXT("bool"), TEXT("Include current input override values (default: true)"))
			.Build());

	// --- Wave 5: Dynamic Inputs (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_dynamic_input"), TEXT("Attach a dynamic input script to a module input pin"),
		FMonolithActionHandler::CreateStatic(&HandleAddDynamicInput),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Target module input name"))
			.Required(TEXT("dynamic_input_script"), TEXT("string"), TEXT("Asset path to the dynamic input script"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_dynamic_input_value"), TEXT("Set an input value on a dynamic input node"),
		FMonolithActionHandler::CreateStatic(&HandleSetDynamicInputValue),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("dynamic_input_node"), TEXT("string"), TEXT("GUID of the dynamic input node (from add_dynamic_input)"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name on the dynamic input"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("search_dynamic_inputs"), TEXT("Browse available dynamic input scripts with optional type filtering"),
		FMonolithActionHandler::CreateStatic(&HandleSearchDynamicInputs),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("Keyword search"))
			.Optional(TEXT("input_type"), TEXT("string"), TEXT("Filter by compatible output type (float, LinearColor, Vector)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results (default: 20)"))
			.Build());

	// --- Wave 6: Advanced (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_event_handler"), TEXT("Add an inter-emitter event handler (death, collision, location events)"),
		FMonolithActionHandler::CreateStatic(&HandleAddEventHandler),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Target emitter that receives the event"))
			.Required(TEXT("event_name"), TEXT("string"), TEXT("Event name (CollisionEvent, DeathEvent, LocationEvent)"))
			.Optional(TEXT("source_emitter"), TEXT("string"), TEXT("Source emitter (omit for self-events)"))
			.Optional(TEXT("execution_mode"), TEXT("string"), TEXT("every_particle, spawned_particles, single_particle (default: every_particle)"))
			.Optional(TEXT("max_events_per_frame"), TEXT("integer"), TEXT("Max events per frame (default: 0 = unlimited)"))
			.Optional(TEXT("spawn_number"), TEXT("integer"), TEXT("Spawn number for spawned_particles mode (default: 0)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("validate_system"), TEXT("Pre-compile validation: check for common misconfigurations"),
		FMonolithActionHandler::CreateStatic(&HandleValidateSystem),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_simulation_stage"), TEXT("Add a simulation stage to a GPU emitter (stub — private API)"),
		FMonolithActionHandler::CreateStatic(&HandleAddSimulationStage),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Stage name"))
			.Optional(TEXT("iteration_source"), TEXT("string"), TEXT("particles or data_interface (default: particles)"))
			.Optional(TEXT("num_iterations"), TEXT("integer"), TEXT("Number of iterations (default: 1)"))
			.Build());
}

// ============================================================================
// System Actions (8)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	// Accept common alias names for the emitter asset path
	FString EmitterAssetPath = Params->GetStringField(TEXT("emitter_asset"));
	if (EmitterAssetPath.IsEmpty()) EmitterAssetPath = Params->GetStringField(TEXT("emitter_path"));
	if (EmitterAssetPath.IsEmpty()) EmitterAssetPath = Params->GetStringField(TEXT("template"));
	if (EmitterAssetPath.IsEmpty()) EmitterAssetPath = Params->GetStringField(TEXT("template_path"));
	if (EmitterAssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param 'emitter_asset': provide a NiagaraEmitter asset path"));
	FString EmitterName = Params->HasField(TEXT("name")) ? Params->GetStringField(TEXT("name")) : FString();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraEmitter* EmitterAsset = FMonolithAssetUtils::LoadAssetByPath<UNiagaraEmitter>(EmitterAssetPath);
	if (!EmitterAsset) return FMonolithActionResult::Error(FString::Printf(
		TEXT("Failed to load emitter asset '%s'. Ensure path points to a NiagaraEmitter (not a NiagaraSystem)."), *EmitterAssetPath));

	// Validate the emitter has versions (empty version array causes array-out-of-bounds in AddEmitterHandle)
	if (EmitterAsset->GetAllAvailableVersions().Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Emitter asset '%s' has no available versions. It may be corrupted or not a valid emitter."), *EmitterAssetPath));
	}

	int32 HandleCountBefore = System->GetEmitterHandles().Num();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddEmitter", "Add Emitter"));

	// Use engine's full add-emitter path: AddEmitterHandle + RebuildEmitterNodes + SynchronizeOverviewGraph.
	// Calling AddEmitterHandle alone leaves the emitter without graph nodes ("Data missing please force a recompile").
	const FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System, *EmitterAsset, EmitterAsset->GetExposedVersion().VersionGuid);

	GEditor->EndTransaction();

	// Validate the emitter was actually added (engine can silently fail)
	if (System->GetEmitterHandles().Num() <= HandleCountBefore || !NewHandleId.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to add emitter from '%s'. The asset may not be a valid NiagaraEmitter or may be incompatible."),
			*EmitterAssetPath));
	}

	// Mark the package dirty so the editor tracks unsaved changes and includes it in autosave.
	// Modify() alone only handles undo — MarkPackageDirty() is what actually flags the asset for save.
	System->MarkPackageDirty();

	// Save the package to disk immediately. Without this, the Niagara editor may reload from disk
	// (which has the old/empty state) and the added emitter will appear to vanish.
	UPackage* SystemPkg = System->GetPackage();
	FString PackageFilename;
	if (SystemPkg && FPackageName::TryConvertLongPackageNameToFilename(SystemPkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(SystemPkg, System, *PackageFilename, SaveArgs);
	}

	// RequestCompile(false) intentionally removed: AddEmitterToSystem already triggers graph rebuild,
	// and async compile causes a timing race in the spec flow where HandleAddModule runs before
	// the ParameterMap chain is wired (bHasChainSource == false). The spec flow forces a synchronous
	// compile via RequestCompile(true) + WaitForCompilationComplete() after each emitter add instead.

	// If a custom name was requested, rename the emitter handle (AddEmitterToSystem auto-generates names)
	if (!EmitterName.IsEmpty())
	{
		for (int32 i = 0; i < System->GetEmitterHandles().Num(); ++i)
		{
			if (System->GetEmitterHandles()[i].GetId() == NewHandleId)
			{
				System->GetEmitterHandles()[i].SetName(FName(*EmitterName), *System);
				break;
			}
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("handle_id"), NewHandleId.ToString());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter handle not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[Index];
	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemoveEmitter", "Remove Emitter"));
	System->Modify();
	System->RemoveEmitterHandle(Handle);
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(TEXT("Emitter removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleDuplicateEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString SourceHandleId = Params->GetStringField(TEXT("source_emitter"));
	if (SourceHandleId.IsEmpty()) SourceHandleId = Params->GetStringField(TEXT("emitter"));
	FString NewName = Params->HasField(TEXT("new_name")) ? Params->GetStringField(TEXT("new_name")) : FString();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, SourceHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Source emitter not found"));

	const FNiagaraEmitterHandle& Src = System->GetEmitterHandles()[Index];
	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "DupEmitter", "Duplicate Emitter"));
	System->Modify();

	FName DupName = NewName.IsEmpty() ? FName(*(Src.GetName().ToString() + TEXT("_Copy"))) : FName(*NewName);
	FNiagaraEmitterHandle NewHandle = System->DuplicateEmitterHandle(Src, DupName);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("handle_id"), NewHandle.GetId().ToString());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEmitterEnabled(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	bool bEnabled = Params->GetBoolField(TEXT("enabled"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter handle not found"));

	TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetEmEnabled", "Set Emitter Enabled"));
	System->Modify();
	Handles[Index].SetIsEnabled(bEnabled, *System, true);
	GEditor->EndTransaction();

	return SuccessStr(bEnabled ? TEXT("Emitter enabled") : TEXT("Emitter disabled"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleReorderEmitters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	const TArray<TSharedPtr<FJsonValue>>& OrderArr = Params->GetArrayField(TEXT("order"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	const TArray<FNiagaraEmitterHandle>& Current = System->GetEmitterHandles();
	if (OrderArr.Num() != Current.Num())
		return FMonolithActionResult::Error(FString::Printf(TEXT("Provided %d IDs but system has %d emitters"), OrderArr.Num(), Current.Num()));

	TArray<FNiagaraEmitterHandle> NewOrder;
	NewOrder.Reserve(OrderArr.Num());
	for (const TSharedPtr<FJsonValue>& V : OrderArr)
	{
		int32 Idx = FindEmitterHandleIndex(System, V->AsString());
		if (Idx == INDEX_NONE) return FMonolithActionResult::Error(FString::Printf(TEXT("Handle '%s' not found"), *V->AsString()));
		NewOrder.Add(Current[Idx]);
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "ReorderEm", "Reorder Emitters"));
	System->Modify();
	System->GetEmitterHandles() = MoveTemp(NewOrder);
	System->PostEditChange();
	System->MarkPackageDirty();
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(TEXT("Emitters reordered"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString PropertyName = Params->GetStringField(TEXT("property"));
	if (PropertyName.IsEmpty()) PropertyName = Params->GetStringField(TEXT("property_name"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter handle not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[Index].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetEmProp", "Set Emitter Property"));
	System->Modify();
	bool bOk = false;

	if (PropertyName == TEXT("SimTarget") || PropertyName == TEXT("sim_target"))
	{
		FString V = JV->AsString();
		ENiagaraSimTarget NewTarget = ED->SimTarget;
		if (V.Equals(TEXT("CPU"), ESearchCase::IgnoreCase) || V.Equals(TEXT("CPUSim"), ESearchCase::IgnoreCase))
			{ NewTarget = ENiagaraSimTarget::CPUSim; bOk = true; }
		else if (V.Equals(TEXT("GPU"), ESearchCase::IgnoreCase) || V.Equals(TEXT("GPUComputeSim"), ESearchCase::IgnoreCase))
			{ NewTarget = ENiagaraSimTarget::GPUComputeSim; bOk = true; }

		if (!bOk)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("SimTarget: invalid value '%s'. Accepted: CPU, CPUSim, GPU, GPUComputeSim"), *V));
		}

		if (NewTarget != ED->SimTarget)
		{
			FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[Index];
			FProperty* SimTargetProp = FindFProperty<FProperty>(
				FVersionedNiagaraEmitterData::StaticStruct(),
				GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, SimTarget));

			// PreEditChange before modifying (matches engine's SNiagaraSimTargetToggle pattern)
			Handle.GetInstance().Emitter->PreEditChange(SimTargetProp);
			Handle.GetInstance().Emitter->Modify();
			ED->SimTarget = NewTarget;

			// Fire PostEditChangeVersionedProperty so the graph source is marked dirty.
			FPropertyChangedEvent PCE(SimTargetProp);
			Handle.GetInstance().Emitter->PostEditChangeVersionedProperty(PCE, Handle.GetInstance().Version);

			// Request compile for this specific emitter (mirrors engine's SNiagaraSimTargetToggle)
			UNiagaraSystem::RequestCompileForEmitter(Handle.GetInstance());
		}
	}
	else if (PropertyName == TEXT("bLocalSpace") || PropertyName == TEXT("local_space"))
	{
		ED->bLocalSpace = JV->AsBool(); bOk = true;
		// bLocalSpace affects compilation — notify via PostEditChangeVersionedProperty
		FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[Index];
		FProperty* Prop = FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, bLocalSpace));
		FPropertyChangedEvent PCE(Prop);
		Handle.GetInstance().Emitter->PostEditChangeVersionedProperty(PCE, Handle.GetInstance().Version);
	}
	else if (PropertyName == TEXT("bDeterminism") || PropertyName == TEXT("determinism"))
	{
		ED->bDeterminism = JV->AsBool(); bOk = true;
		FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[Index];
		FProperty* Prop = FindFProperty<FProperty>(FVersionedNiagaraEmitterData::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVersionedNiagaraEmitterData, bDeterminism));
		FPropertyChangedEvent PCE(Prop);
		Handle.GetInstance().Emitter->PostEditChangeVersionedProperty(PCE, Handle.GetInstance().Version);
	}
	else if (PropertyName == TEXT("CalculateBoundsMode") || PropertyName == TEXT("calculate_bounds_mode") || PropertyName == TEXT("bounds_mode"))
	{
		FString V = JV->AsString();
		if (V == TEXT("Dynamic")) { ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Dynamic; bOk = true; }
		else if (V == TEXT("Fixed")) { ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed; bOk = true; }
		else if (V == TEXT("Programmable")) { ED->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Programmable; bOk = true; }
	}
	else if (PropertyName == TEXT("RandomSeed") || PropertyName == TEXT("random_seed"))
	{ ED->RandomSeed = static_cast<int32>(JV->AsNumber()); bOk = true; }
	else if (PropertyName == TEXT("AllocationMode") || PropertyName == TEXT("allocation_mode"))
	{
		FString V = JV->AsString();
		if (V == TEXT("AutomaticEstimate")) { ED->AllocationMode = EParticleAllocationMode::AutomaticEstimate; bOk = true; }
		else if (V == TEXT("ManualEstimate")) { ED->AllocationMode = EParticleAllocationMode::ManualEstimate; bOk = true; }
		else if (V == TEXT("FixedCount")) { ED->AllocationMode = EParticleAllocationMode::FixedCount; bOk = true; }
	}
	else if (PropertyName == TEXT("PreAllocationCount") || PropertyName == TEXT("pre_allocation_count"))
	{ ED->PreAllocationCount = static_cast<int32>(JV->AsNumber()); bOk = true; }
	else if (PropertyName == TEXT("bRequiresPersistentIDs") || PropertyName == TEXT("requires_persistent_ids"))
	{ ED->bRequiresPersistentIDs = JV->AsBool(); bOk = true; }
	else if (PropertyName == TEXT("MaxGPUParticlesSpawnPerFrame") || PropertyName == TEXT("max_gpu_particles_spawn_per_frame"))
	{ ED->MaxGPUParticlesSpawnPerFrame = static_cast<int32>(JV->AsNumber()); bOk = true; }

	GEditor->EndTransaction();
	if (bOk) System->RequestCompile(false);
	return bOk ? SuccessStr(TEXT("Property set")) : FMonolithActionResult::Error(TEXT("Unknown property"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleRequestCompile(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	bool bForce = Params->HasField(TEXT("force")) && Params->GetBoolField(TEXT("force"));
	bool bSync = Params->HasField(TEXT("synchronous")) && Params->GetBoolField(TEXT("synchronous"));

	System->RequestCompile(bForce);
	if (bSync)
	{
		System->WaitForCompilationComplete();
	}
	return SuccessStr(TEXT("Compile requested"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	FString TemplatePath = Params->HasField(TEXT("template")) ? Params->GetStringField(TEXT("template")) : FString();

	if (!TemplatePath.IsEmpty())
	{
		UNiagaraSystem* Template = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(TemplatePath);
		if (!Template) return FMonolithActionResult::Error(TEXT("Failed to load template"));

		FString PackagePath, AssetName;
		int32 LastSlash;
		if (!SavePath.FindLastChar('/', LastSlash)) return FMonolithActionResult::Error(TEXT("Invalid save path"));
		PackagePath = SavePath.Left(LastSlash);
		AssetName = SavePath.Mid(LastSlash + 1);

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Dup = AT.DuplicateAsset(AssetName, PackagePath, Template);
		if (!Dup) return FMonolithActionResult::Error(TEXT("Failed to duplicate template"));
		return SuccessStr(Dup->GetPathName());
	}

	FString PackagePath, AssetName;
	int32 LastSlash;
	if (!SavePath.FindLastChar('/', LastSlash)) return FMonolithActionResult::Error(TEXT("Invalid save path"));
	PackagePath = SavePath.Left(LastSlash);
	AssetName = SavePath.Mid(LastSlash + 1);

	FString FullPath = PackagePath / AssetName;
	UPackage* Pkg = CreatePackage(*FullPath);
	if (!Pkg) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	UNiagaraSystem* NS = NewObject<UNiagaraSystem>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!NS) return FMonolithActionResult::Error(TEXT("Failed to create system"));

	// Must initialize via factory method — raw NewObject leaves internal arrays/editor data uninitialized,
	// causing array-out-of-bounds crashes when AddEmitterHandle is called later.
	UNiagaraSystemFactoryNew::InitializeSystem(NS, true);

	FAssetRegistryModule::AssetCreated(NS);
	Pkg->MarkPackageDirty();

	// Save to disk immediately so the .uasset file exists. Without this, the Niagara editor will
	// either fail to open the asset or reload from a missing disk file and show an empty system.
	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(Pkg, NS, *PackageFilename, SaveArgs);
	}

	return SuccessStr(NS->GetPathName());
}

// ============================================================================
// Module Actions (12)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetOrderedModules(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ScriptUsage = Params->GetStringField(TEXT("usage"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Determine which usages to query
	TArray<ENiagaraScriptUsage> UsagesToQuery;
	if (ScriptUsage.IsEmpty())
	{
		// No usage specified — return ALL stages
		UsagesToQuery = {
			ENiagaraScriptUsage::EmitterSpawnScript, ENiagaraScriptUsage::EmitterUpdateScript,
			ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript,
		};
	}
	else
	{
		ENiagaraScriptUsage Usage;
		if (!ResolveScriptUsage(ScriptUsage, Usage))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unrecognized usage '%s'. Valid values: system_spawn, system_update, emitter_spawn, emitter_update, particle_spawn (or spawn), particle_update (or update)"),
				*ScriptUsage));
		}
		UsagesToQuery.Add(Usage);
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (ENiagaraScriptUsage Usage : UsagesToQuery)
	{
		UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId, Usage);
		if (!OutputNode) continue;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);

		FString UsageName = UsageToString(Usage);
		for (int32 i = 0; i < ModuleNodes.Num(); ++i)
		{
			UNiagaraNodeFunctionCall* N = ModuleNodes[i];
			if (!N) continue;
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("node_guid"), N->NodeGuid.ToString());
			M->SetStringField(TEXT("function_name"), N->GetFunctionName());
			M->SetNumberField(TEXT("index"), i);
			M->SetStringField(TEXT("usage"), UsageName);
			TOptional<bool> bEn = MonolithNiagaraHelpers::GetModuleIsEnabled(*N);
			M->SetBoolField(TEXT("enabled"), bEn.IsSet() ? bEn.GetValue() : true);
			if (N->FunctionScript) M->SetStringField(TEXT("script_path"), N->FunctionScript->GetPathName());
			Arr.Add(MakeShared<FJsonValueObject>(M));
		}
	}
	return SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *EmitterHandleId));

	ENiagaraScriptUsage FoundUsage = ENiagaraScriptUsage::ParticleUpdateScript;
	UNiagaraNodeFunctionCall* ModuleNode = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!ModuleNode) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Use the engine's full input enumeration (includes data inputs from the script, not just pins on the node)
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*ModuleNode, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		// System-level module (no emitter) — use system resolver
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*ModuleNode, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// Fallback for CustomHlsl modules: GetStackFunctionInputs returns empty because the
	// parameter map history doesn't contain our typed inputs (they're function-locals, not
	// Module.-prefixed map entries). Read the FunctionCall's typed input pins directly instead.
	if (Inputs.Num() == 0)
	{
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		for (UEdGraphPin* Pin : ModuleNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			FNiagaraTypeDefinition PinType = Schema->PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
			if (Pin->PinName.IsNone() || Pin->PinName == TEXT("Add")) continue;
			Inputs.Add(FNiagaraVariable(PinType, Pin->PinName));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		// Strip "Module." prefix for consistent short names across read/write actions
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(Input.GetName());
		TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
		IO->SetStringField(TEXT("name"), ShortName.ToString());
		IO->SetStringField(TEXT("type"), Input.GetType().GetName());

		// Use full name (with Module. prefix) for aliased handle — stripped name produces wrong namespace
		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(Input.GetName()), ModuleNode);
		UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*ModuleNode, AH);
		if (OP)
		{
			IO->SetStringField(TEXT("override_value"), OP->DefaultValue);
			IO->SetBoolField(TEXT("has_override"), true);
			if (OP->LinkedTo.Num() > 0)
			{
				IO->SetBoolField(TEXT("is_linked"), true);
				if (UNiagaraNodeInput* LI = Cast<UNiagaraNodeInput>(OP->LinkedTo[0]->GetOwningNode()))
				{
					IO->SetStringField(TEXT("linked_parameter"), LI->Input.GetName().ToString());
					// For DI inputs (curves, etc.), serialize the actual DI data
					// UNiagaraNodeInput::GetDataInterface() is not exported — access via UProperty reflection
					FObjectProperty* DIProp = FindFProperty<FObjectProperty>(LI->GetClass(), TEXT("DataInterface"));
					UNiagaraDataInterface* DI = DIProp ? Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue(DIProp->ContainerPtrToValuePtr<void>(LI))) : nullptr;
					if (DI)
					{
						TSharedPtr<FJsonObject> CurveData = MonolithNiagaraHelpers::SerializeDICurveData(DI);
						if (CurveData.IsValid())
						{
							// Check if curve data has any actual keys (not just empty arrays)
							bool bHasKeys = false;
							for (const auto& Field : CurveData->Values)
							{
								if (Field.Value->Type == EJson::Array && Field.Value->AsArray().Num() > 0)
								{
									bHasKeys = true;
									break;
								}
							}
							IO->SetObjectField(TEXT("di_data"), CurveData);
							if (!bHasKeys)
							{
								IO->SetBoolField(TEXT("di_using_defaults"), true);
							}
						}
					}
				}
			}
		}
		else
		{
			IO->SetBoolField(TEXT("has_override"), false);
		}
		Arr.Add(MakeShared<FJsonValueObject>(IO));
	}

	// Enumerate static switch inputs (separate engine API — not part of ParameterMap traversal)
	{
		TArray<UEdGraphPin*> StaticSwitchPins;
		TSet<UEdGraphPin*> HiddenSwitchPins;
		if (EmitterIdx != INDEX_NONE)
		{
			FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
			FCompileConstantResolver Resolver(VE, FoundUsage);
			FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
				*ModuleNode, StaticSwitchPins, HiddenSwitchPins, Resolver);
		}
		else
		{
			FCompileConstantResolver Resolver(System, FoundUsage);
			FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
				*ModuleNode, StaticSwitchPins, HiddenSwitchPins, Resolver);
		}
		const UEdGraphSchema_Niagara* SwitchSchema = GetDefault<UEdGraphSchema_Niagara>();
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
			IO->SetStringField(TEXT("name"), SwitchPin->GetFName().ToString());
			FNiagaraTypeDefinition PinType = SwitchSchema->PinToTypeDefinition(SwitchPin);
			IO->SetStringField(TEXT("type"), PinType.GetName());
			IO->SetStringField(TEXT("override_value"), SwitchPin->DefaultValue);
			IO->SetBoolField(TEXT("has_override"), true);
			IO->SetBoolField(TEXT("is_static_switch"), true);
			IO->SetBoolField(TEXT("is_hidden"), HiddenSwitchPins.Contains(SwitchPin));
			Arr.Add(MakeShared<FJsonValueObject>(IO));
		}
	}

	return SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script) return FMonolithActionResult::Error(TEXT("Failed to load script"));

	UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Src || !Src->NodeGraph) return FMonolithActionResult::Error(TEXT("No graph available"));

	UNiagaraGraph* Graph = Src->NodeGraph;
	TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
	Res->SetStringField(TEXT("script_path"), ScriptPath);
	Res->SetStringField(TEXT("script_usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage())));

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	TArray<UEdGraphNode*> AllNodes;
	Graph->GetNodesOfClass<UEdGraphNode>(AllNodes);
	for (UEdGraphNode* Node : AllNodes)
	{
		TSharedRef<FJsonObject> NO = MakeShared<FJsonObject>();
		NO->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
		NO->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NO->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NO->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		NO->SetNumberField(TEXT("pos_y"), Node->NodePosY);
		if (UNiagaraNodeFunctionCall* FN = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			NO->SetStringField(TEXT("function_name"), FN->GetFunctionName());
			if (FN->FunctionScript) NO->SetStringField(TEXT("function_script"), FN->FunctionScript->GetPathName());
		}
		TArray<TSharedPtr<FJsonValue>> PinsArr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PO->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
			PO->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
			PO->SetStringField(TEXT("default_value"), Pin->DefaultValue);
			PO->SetNumberField(TEXT("linked_count"), Pin->LinkedTo.Num());
			PinsArr.Add(MakeShared<FJsonValueObject>(PO));
		}
		NO->SetArrayField(TEXT("pins"), PinsArr);
		NodesArr.Add(MakeShared<FJsonValueObject>(NO));
	}
	Res->SetArrayField(TEXT("nodes"), NodesArr);
	return SuccessObj(Res);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ScriptUsage = Params->GetStringField(TEXT("usage"));
	FString ModuleScriptPath = Params->GetStringField(TEXT("module_script"));
	int32 Index = Params->HasField(TEXT("index")) ? static_cast<int32>(Params->GetNumberField(TEXT("index"))) : -1;

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraScript* ModScript = LoadObject<UNiagaraScript>(nullptr, *ModuleScriptPath);
	if (!ModScript)
	{
		// Wave 6.4: fuzzy suggestions on module load failure
		FString RequestedName = FPaths::GetBaseFilename(ModuleScriptPath);
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter SugFilter;
		SugFilter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
		SugFilter.bRecursiveClasses = true;
		TArray<FAssetData> SugAssets;
		AR.GetAssets(SugFilter, SugAssets);
		TArray<FString> Suggestions;
		for (const FAssetData& AD : SugAssets)
		{
			FString N = AD.AssetName.ToString();
			if (N.Contains(RequestedName, ESearchCase::IgnoreCase) || RequestedName.Contains(N, ESearchCase::IgnoreCase))
			{
				Suggestions.Add(FString::Printf(TEXT("%s (%s)"), *N, *AD.GetSoftObjectPath().ToString()));
				if (Suggestions.Num() >= 5) break;
			}
		}
		if (Suggestions.Num() > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to load module script '%s'. Did you mean: %s"),
				*ModuleScriptPath, *FString::Join(Suggestions, TEXT("; "))));
		}
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load module script '%s'"), *ModuleScriptPath));
	}

	ENiagaraScriptUsage Usage;
	if (!ResolveScriptUsage(ScriptUsage, Usage))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unrecognized usage '%s'. Valid values: system_spawn, system_update, emitter_spawn, emitter_update, particle_spawn (or spawn), particle_update (or update)"),
			*ScriptUsage));
	}
	UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId, Usage);
	if (!OutputNode) return FMonolithActionResult::Error(TEXT("No output node"));

	// Bug 2 guard: AddScriptModuleToStack asserts StackNodeGroups.Num() >= 2, which means the
	// output node must have a stack-flow input pin with at least one connection (the chain source
	// node). If it has none the graph is invalid and the assert will crash the editor — return
	// an error instead.
	{
		bool bHasChainSource = false;
		for (UEdGraphPin* P : OutputNode->Pins)
		{
			if (P->Direction == EGPD_Input && P->LinkedTo.Num() > 0)
			{
				bHasChainSource = true;
				break;
			}
		}
		// A freshly-created graph may have an unconnected output node. In that case the graph has
		// only 1 group and the stack utility will assert. Bail out with a clear message.
		if (!bHasChainSource)
		{
			return FMonolithActionResult::Error(TEXT(
				"Stack graph is not fully initialized — the output node has no upstream connection. "
				"This can happen immediately after create_system. Call request_compile first to let "
				"Niagara initialize the graph, then retry add_module."));
		}
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddMod", "Add Module"));
	System->Modify();
	UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModScript, *OutputNode, Index);
	GEditor->EndTransaction();

	if (!NewNode) return FMonolithActionResult::Error(TEXT("AddScriptModuleToStack failed"));
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	FGuid EmitterGuid;
	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx != INDEX_NONE) EmitterGuid = System->GetEmitterHandles()[EIdx].GetId();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemMod", "Remove Module"));
	System->Modify();
	MonolithNiagaraHelpers::RemoveModuleFromStack(*System, EmitterGuid, *MN);
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(TEXT("Module removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleMoveModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	int32 NewIndex = static_cast<int32>(Params->GetNumberField(TEXT("new_index")));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId, FoundUsage);
	if (!OutputNode) return FMonolithActionResult::Error(TEXT("No output node"));

	TArray<UNiagaraNodeFunctionCall*> Mods;
	MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, Mods);
	int32 CurIdx = Mods.IndexOfByKey(MN);
	if (CurIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Module not in ordered list"));
	if (CurIdx == NewIndex) return SuccessStr(TEXT("Already at target index"));

	NewIndex = FMath::Clamp(NewIndex, 0, Mods.Num() - 1);

	// Strategy: remove the module from its current position, then re-add it at the target index.
	// This leverages the engine's own AddScriptModuleToStack which correctly handles all graph
	// wiring (stack-flow pins, node groups, etc.). Hand-rolling pin manipulation is fragile because
	// Niagara uses FStackNodeGroup with multiple nodes per module, not a simple pin chain.

	UNiagaraScript* ModScript = MN->FunctionScript;
	if (!ModScript) return FMonolithActionResult::Error(TEXT("Module has no script reference"));

	FGuid EmitterGuid;
	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx != INDEX_NONE) EmitterGuid = System->GetEmitterHandles()[EIdx].GetId();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "MoveMod", "Move Module"));
	System->Modify();

	// Step 1: Remove from current position
	MonolithNiagaraHelpers::RemoveModuleFromStack(*System, EmitterGuid, *MN);

	// Step 2: Re-add at target position using the engine API
	UNiagaraNodeFunctionCall* NewNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModScript, *OutputNode, NewIndex);

	GEditor->EndTransaction();

	if (!NewNode)
	{
		// The remove succeeded but re-add failed — module is lost. Log clearly.
		System->RequestCompile(false);
		return FMonolithActionResult::Error(TEXT("Module was removed but could not be re-added at target index. The module may need to be re-added manually."));
	}

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("new_node_guid"), NewNode->NodeGuid.ToString());
	R->SetNumberField(TEXT("new_index"), NewIndex);
	R->SetStringField(TEXT("status"), TEXT("Module moved"));
	R->SetStringField(TEXT("warning"), TEXT("Move removes and re-adds the module, so any input overrides are lost. Re-apply them with set_module_input_value after moving."));
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleEnabled(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	bool bEnabled = Params->GetBoolField(TEXT("enabled"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetModEn", "Set Module Enabled"));
	System->Modify();
	FNiagaraStackGraphUtilities::SetModuleIsEnabled(*MN, bEnabled);
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(bEnabled ? TEXT("Module enabled") : TEXT("Module disabled"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module"));
	FString InputName = Params->GetStringField(TEXT("input"));
	if (InputName.IsEmpty()) InputName = Params->GetStringField(TEXT("input_name"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *EmitterHandleId));

	ENiagaraScriptUsage FoundUsage = ENiagaraScriptUsage::ParticleUpdateScript;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Use the engine's full input enumeration (matches HandleGetModuleInputs)
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// Fallback for CustomHlsl modules: GetStackFunctionInputs returns empty because typed
	// inputs aren't Module.-prefixed map entries. Read FunctionCall pins directly.
	bool bCustomHlslFallback = false;
	if (Inputs.Num() == 0)
	{
		bCustomHlslFallback = true;
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		for (UEdGraphPin* Pin : MN->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			FNiagaraTypeDefinition PinType = Schema->PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
			if (Pin->PinName.IsNone() || Pin->PinName == TEXT("Add")) continue;
			Inputs.Add(FNiagaraVariable(PinType, Pin->PinName));
		}
	}

	// Match input by short name (strip Module. prefix for comparison)
	FNiagaraTypeDefinition InputType;
	bool bInputFound = false;
	FName MatchedFullName;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		if (ShortName == FName(*InputName)) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}

	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : Inputs) { ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString()); }
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' not found on module. Valid inputs: [%s]"),
			*InputName, *FString::Join(ValidNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetModIn", "Set Module Input"));
	System->Modify();

	UEdGraphPin* TargetPin = nullptr;
	if (bCustomHlslFallback)
	{
		// CustomHlsl fallback: set DefaultValue directly on the FunctionCall's typed input pin.
		// No ParameterMapSet override node exists for these modules.
		for (UEdGraphPin* Pin : MN->Pins)
		{
			if (Pin->Direction == EGPD_Input && Pin->PinName == MatchedFullName)
			{
				TargetPin = Pin;
				break;
			}
		}
		if (!TargetPin)
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(TEXT("Failed to find pin on FunctionCall node"));
		}
		if (TargetPin->LinkedTo.Num() > 0)
		{
			TargetPin->BreakAllPinLinks();
		}
	}
	else
	{
		// Standard path: use the ParameterMap override pin system
		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(MatchedFullName), MN);

		// UE 5.7 FIX: 5-param version of GetOrCreateStackFunctionInputOverridePin
		UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			*MN, AH, InputType, FGuid(), FGuid());

		// Guard: break existing links so the literal DefaultValue actually takes effect
		if (OverridePin.LinkedTo.Num() > 0)
		{
			OverridePin.BreakAllPinLinks();
		}
		TargetPin = &OverridePin;
	}

	FString ValStr;
	if (JV->Type == EJson::Number) ValStr = FString::SanitizeFloat(JV->AsNumber());
	else if (JV->Type == EJson::Boolean) ValStr = JV->AsBool() ? TEXT("true") : TEXT("false");
	else if (JV->Type == EJson::String) ValStr = JV->AsString();
	else if (JV->Type == EJson::Object)
	{
		TSharedPtr<FJsonObject> O = JV->AsObject();
		if (O->HasField(TEXT("x")))
		{
			double X = O->GetNumberField(TEXT("x")), Y = O->GetNumberField(TEXT("y"));
			double Z = O->HasField(TEXT("z")) ? O->GetNumberField(TEXT("z")) : 0.0;
			double W = O->HasField(TEXT("w")) ? O->GetNumberField(TEXT("w")) : 0.0;
			if (O->HasField(TEXT("w"))) ValStr = FString::Printf(TEXT("%f,%f,%f,%f"), X, Y, Z, W);
			else if (O->HasField(TEXT("z"))) ValStr = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
			else ValStr = FString::Printf(TEXT("%f,%f"), X, Y);
		}
		else if (O->HasField(TEXT("r")))
		{
			double R2 = O->GetNumberField(TEXT("r")), G = O->GetNumberField(TEXT("g"));
			double B = O->GetNumberField(TEXT("b")), A = O->HasField(TEXT("a")) ? O->GetNumberField(TEXT("a")) : 1.0;
			ValStr = FString::Printf(TEXT("%f,%f,%f,%f"), R2, G, B, A);
		}
		else
		{
			TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&ValStr);
			FJsonSerializer::Serialize(O.ToSharedRef(), W);
		}
	}
	else ValStr = JsonValueToString(JV);

	TargetPin->DefaultValue = ValStr;
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(FString::Printf(TEXT("Set input '%s' = '%s'"), *InputName, *ValStr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module"));
	FString InputName = Params->GetStringField(TEXT("input"));
	if (InputName.IsEmpty()) InputName = Params->GetStringField(TEXT("input_name"));
	FString BindingPath = Params->GetStringField(TEXT("binding"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *EmitterHandleId));

	ENiagaraScriptUsage FoundUsage = ENiagaraScriptUsage::ParticleUpdateScript;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Use the engine's full input enumeration (matches HandleGetModuleInputs)
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// Match input by short name (strip Module. prefix for comparison)
	FNiagaraTypeDefinition InputType;
	bool bInputFound = false;
	FName MatchedFullName;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		if (ShortName == FName(*InputName)) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}

	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : Inputs) { ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString()); }
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' not found on module. Valid inputs: [%s]"),
			*InputName, *FString::Join(ValidNames, TEXT(", "))));
	}

	FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedFullName), MN);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetModBind", "Set Module Binding"));
	System->Modify();

	// UE 5.7 FIX: 5-param version
	UEdGraphPin& OP = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*MN, AH, InputType, FGuid(), FGuid());

	// Guard: SetLinkedParameterValueForFunctionInput expects the pin to be on a
	// UNiagaraNodeParameterMapSet (override) node. If the pin is on the FunctionCall
	// itself (static switch input), binding is not supported — would crash with CastChecked.
	if (OP.GetOwningNode() == MN)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' is a static switch and cannot be bound to a parameter. Use set_module_input_value instead."), *InputName));
	}

	// Guard: engine asserts the pin has no existing links
	if (OP.LinkedTo.Num() > 0)
	{
		// Break existing links so SetLinkedParameterValueForFunctionInput can proceed
		OP.BreakAllPinLinks();
	}

	FNiagaraVariable LinkedParam(InputType, FName(*BindingPath));
	UNiagaraGraph* Graph = MN->GetNiagaraGraph();
	TSet<FNiagaraVariableBase> KnownParams;
	if (Graph) MonolithNiagaraHelpers::GetParametersForContext(Graph, *System, KnownParams);
	FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(OP, LinkedParam, KnownParams);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(FString::Printf(TEXT("Bound '%s' to '%s'"), *InputName, *BindingPath));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputDI(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module"));
	FString InputName = Params->GetStringField(TEXT("input"));
	if (InputName.IsEmpty()) InputName = Params->GetStringField(TEXT("input_name"));
	FString DIClass = Params->GetStringField(TEXT("di_class"));
	TSharedPtr<FJsonObject> DIConfig;
	if (Params->HasField(TEXT("config")))
	{
		TSharedPtr<FJsonValue> ConfigField = Params->TryGetField(TEXT("config"));
		if (ConfigField.IsValid() && ConfigField->Type == EJson::Object)
		{
			DIConfig = ConfigField->AsObject();
		}
		else if (ConfigField.IsValid() && ConfigField->Type == EJson::String)
		{
			// Legacy: config as JSON string
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ConfigField->AsString());
			FJsonSerializer::Deserialize(Reader, DIConfig);
		}
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *EmitterHandleId));

	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Normalize DI class name — accept any of these forms:
	//   "UNiagaraDataInterfaceCurve"      (full with U prefix)
	//   "NiagaraDataInterfaceCurve"       (no U prefix — exact object name)
	//   "NiagaraDataInterfaceColorCurve"  (exact object name, works)
	//   "CurveLinearColor"                (short name — old prefix logic had word-order bugs)
	//   "Curve"                           (minimal short name)
	//
	// Strategy: strip U prefix, build exact-name candidates, then fall back to
	// GetDerivedClasses fuzzy suffix matching. This is robust against:
	//   1. Word-order mismatches ("CurveLinearColor" → actual class "ColorCurve")
	//   2. FindFirstObject returning null for classes with ambiguous/colliding names
	//   3. Short names that don't map predictably to full class names
	UClass* DIUClass = nullptr;

	// Step 1: normalize input — strip leading "U" if it's the class-prefix U
	FString Stripped = DIClass;
	if (Stripped.StartsWith(TEXT("U")) && Stripped.Len() > 1 && FChar::IsUpper(Stripped[1]))
		Stripped = Stripped.Mid(1);

	// Step 2: build exact-name candidates (most specific first) and try FindFirstObject.
	// These work when the caller supplies the correct full object name.
	TArray<FString> ExactCandidates;
	if (!Stripped.StartsWith(TEXT("NiagaraDataInterface")))
		ExactCandidates.Add(TEXT("NiagaraDataInterface") + Stripped);
	ExactCandidates.AddUnique(Stripped);

	for (const FString& Candidate : ExactCandidates)
	{
		UClass* Found = FindFirstObject<UClass>(*Candidate, EFindFirstObjectOptions::NativeFirst);
		if (Found && Found->IsChildOf<UNiagaraDataInterface>())
		{
			DIUClass = Found;
			break;
		}
	}

	// Step 3: if exact lookup failed, fall back to GetDerivedClasses suffix scan.
	// This handles short/mangled names like "Curve", "CurveLinearColor", "ColorCurve", etc.
	// It's O(N) over all DI subclasses but that's ~30 classes — fine for MCP calls.
	if (!DIUClass)
	{
		// Build a lowercase suffix to match against. Try progressively shorter suffixes:
		// full Stripped, then just the token after the last known prefix.
		FString LowerStripped = Stripped.ToLower();

		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(UNiagaraDataInterface::StaticClass(), DerivedClasses, /*bRecursive=*/true);

		UClass* BestMatch = nullptr;
		for (UClass* DI : DerivedClasses)
		{
			if (!DI || DI->HasAnyClassFlags(CLASS_Abstract))
				continue;
			FString ClassName = DI->GetName(); // e.g. "NiagaraDataInterfaceColorCurve"
			FString LowerClass = ClassName.ToLower();

			// Exact match (case-insensitive) — highest priority
			if (LowerClass == LowerStripped)
			{
				BestMatch = DI;
				break;
			}

			// Suffix match: class name ends with the stripped input
			// e.g. "Curve" matches "NiagaraDataInterfaceCurve" (ends with "Curve")
			// and "ColorCurve" matches "NiagaraDataInterfaceColorCurve"
			if (LowerClass.EndsWith(LowerStripped))
			{
				// Prefer the shortest matching class name — it's the most specific match.
				// "Curve" → NiagaraDataInterfaceCurve (31) wins over NiagaraDataInterfaceVectorCurve (35)
				if (!BestMatch || ClassName.Len() < BestMatch->GetName().Len())
					BestMatch = DI;
			}
		}
		DIUClass = BestMatch;
	}

	if (!DIUClass) return FMonolithActionResult::Error(FString::Printf(
		TEXT("DI class not found (must be a UNiagaraDataInterface subclass). Tried exact: [%s], then fuzzy suffix scan over all DI subclasses. Input was: '%s'"),
		*FString::Join(ExactCandidates, TEXT(", ")), *DIClass));

	// Enumerate all inputs using the engine's full API (includes data + DI inputs from the script)
	ENiagaraScriptUsage FoundUsage = ENiagaraScriptUsage::ParticleUpdateScript;
	FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage); // re-find to get usage
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// Validate input exists on this module (accept both short "Gravity" and full "Module.Gravity")
	// Also accepts space-stripped names ("UniformCurve" matches "Uniform Curve")
	FName InputFName(*InputName);
	FString InputNameNoSpaces = InputName;
	InputNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	bool bFoundInput = false;
	FNiagaraTypeDefinition InputType(DIUClass);
	FName MatchedDIFullName; // store full Module.X name for correct PH construction
	for (const FNiagaraVariable& In : Inputs)
	{
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		// Exact match on short or full name
		bool bMatch = (ShortName == InputFName || In.GetName() == InputFName);
		// Fallback: space-stripped comparison (e.g. "UniformCurve" matches "Uniform Curve")
		if (!bMatch)
		{
			FString ShortNameNoSpaces = ShortName.ToString();
			ShortNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = ShortNameNoSpaces.Equals(InputNameNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch)
		{
			InputType = In.GetType();
			bFoundInput = true;
			MatchedDIFullName = In.GetName();

			// Validate type compatibility: input must already be a DataInterface type
			if (!InputType.IsDataInterface())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Input '%s' is type '%s', not a DataInterface. Use set_module_input_value or set_module_input_binding instead."),
					*InputName, *InputType.GetName()));
			}
			break;
		}
	}
	if (!bFoundInput)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : Inputs) { ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString()); }
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' not found on module '%s'. Valid inputs: [%s]"),
			*InputName, *MN->GetFunctionName(), *FString::Join(ValidNames, TEXT(", "))));
	}

	// Validate DI type compatibility: the resolved DIUClass must match or be a child of
	// the input's expected DI class. This prevents silent type mismatches like assigning
	// NiagaraDataInterfaceCurve (float) to a NiagaraDataInterfaceColorCurve input.
	if (InputType.IsDataInterface())
	{
		UClass* ExpectedDIClass = const_cast<UClass*>(InputType.GetClass());
		if (ExpectedDIClass && !DIUClass->IsChildOf(ExpectedDIClass))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Input '%s' expects DI type '%s', but got '%s'. The provided DI class is not compatible."),
				*InputName, *ExpectedDIClass->GetName(), *DIUClass->GetName()));
		}
	}

	// Use the full Module.X name for correct namespace aliasing
	FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedDIFullName), MN);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetModDI", "Set Module DI"));
	System->Modify();

	// UE 5.7 FIX: 5-param version
	UEdGraphPin& OP = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*MN, AH, InputType, FGuid(), FGuid());

	// Guard: static switch pins live on the FunctionCall node itself — DI set not supported
	if (OP.GetOwningNode() == MN)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' is a static switch and cannot accept a DataInterface. Use set_module_input_value instead."), *InputName));
	}

	// Three-path approach to avoid orphaned nodes (which cause Module.X001 duplicates):
	// Path A: Pin already linked to a DI node with matching class → reuse existing DI (true update-in-place)
	// Path B: Pin linked to wrong type → remove old node + create new
	// Path C: No existing link → create new (first-time set)
	UNiagaraDataInterface* DIInst = nullptr;
	if (OP.LinkedTo.Num() > 0)
	{
		UNiagaraNodeInput* ExistingInputNode = Cast<UNiagaraNodeInput>(OP.LinkedTo[0]->GetOwningNode());
		if (ExistingInputNode)
		{
			// Access DI via UProperty reflection (GetDataInterface() not exported)
			FObjectProperty* DIProp = FindFProperty<FObjectProperty>(ExistingInputNode->GetClass(), TEXT("DataInterface"));
			UNiagaraDataInterface* ExistingDI = DIProp
				? Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue(DIProp->ContainerPtrToValuePtr<void>(ExistingInputNode)))
				: nullptr;

			if (ExistingDI && ExistingDI->GetClass() == DIUClass)
			{
				// Path A: reuse existing DI — no new node needed
				DIInst = ExistingDI;
			}
			else
			{
				// Path B: wrong DI type — remove old node from graph, then create fresh
				UEdGraph* Graph = ExistingInputNode->GetGraph();
				Graph->Modify();
				OP.BreakAllPinLinks();
				Graph->RemoveNode(ExistingInputNode);
			}
		}
		else
		{
			// Non-InputNode link (e.g. ParameterMapGet, FunctionCall for dynamic inputs).
			// Clean up orphaned upstream nodes that have no other output connections.
			UEdGraphNode* UpstreamNode = OP.LinkedTo[0]->GetOwningNode();
			UEdGraph* Graph = UpstreamNode ? UpstreamNode->GetGraph() : nullptr;
			OP.BreakAllPinLinks();
			if (UpstreamNode && Graph && !Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
			{
				// Check if the upstream node has any remaining output connections
				bool bHasOtherOutputs = false;
				for (UEdGraphPin* Pin : UpstreamNode->Pins)
				{
					if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						bHasOtherOutputs = true;
						break;
					}
				}
				if (!bHasOtherOutputs)
				{
					Graph->Modify();
					Graph->RemoveNode(UpstreamNode);
				}
			}
		}
	}

	// Path B/C: create new DI node if we didn't reuse an existing one
	if (!DIInst)
	{
		FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OP, DIUClass, InputName, DIInst);
	}

	bool bCurveConfigApplied = false;
	if (DIInst && DIConfig.IsValid())
	{
		// Clear CurveAsset reference if present — external curve assets override inline keys
		if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DIInst))
		{
			FObjectProperty* CurveAssetProp = FindFProperty<FObjectProperty>(CurveBase->GetClass(), TEXT("CurveAsset"));
			if (CurveAssetProp)
			{
				CurveAssetProp->SetObjectPropertyValue(CurveAssetProp->ContainerPtrToValuePtr<void>(CurveBase), nullptr);
			}
		}

		// Try curve-specific config first (handles keys, red/green/blue/alpha, x/y/z/w)
		bool bCurveApplied = MonolithNiagaraHelpers::ApplyCurveConfig(DIInst, DIConfig);
		bCurveConfigApplied = bCurveApplied;

		// Check if config had curve-like fields that ApplyCurveConfig didn't handle
		if (!bCurveApplied)
		{
			static const TSet<FString> CurveFieldNames = {
				TEXT("keys"), TEXT("curve"),
				TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha"),
				TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")
			};
			bool bHadCurveFields = false;
			for (const auto& Pair : DIConfig->Values)
			{
				if (CurveFieldNames.Contains(Pair.Key)) { bHadCurveFields = true; break; }
			}
			if (bHadCurveFields)
			{
				UE_LOG(LogMonolithNiagara, Warning, TEXT("set_module_input_di: config had curve fields but DI type '%s' didn't match any known curve DI. Curve keys were NOT applied."), *DIUClass->GetName());
			}
		}

		// Fall back to simple property reflection for non-curve properties
		// Only skip curve field names if the DI is actually a curve type — non-curve DIs
		// might legitimately have properties named "x", "y", "red", etc.
		const bool bIsCurveDI = Cast<UNiagaraDataInterfaceCurveBase>(DIInst) != nullptr;
		static const TSet<FString> CurveKeys = {
			TEXT("keys"), TEXT("curve"),
			TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha"),
			TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")
		};
		for (auto& Pair : DIConfig->Values)
		{
			if (bIsCurveDI && CurveKeys.Contains(Pair.Key)) continue;

			FProperty* Prop = DIUClass->FindPropertyByName(FName(*Pair.Key));
			if (!Prop) continue;
			void* Addr = Prop->ContainerPtrToValuePtr<void>(DIInst);
			if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) FP->SetPropertyValue(Addr, static_cast<float>(Pair.Value->AsNumber()));
			else if (FIntProperty* IP = CastField<FIntProperty>(Prop)) IP->SetPropertyValue(Addr, static_cast<int32>(Pair.Value->AsNumber()));
			else if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) BP->SetPropertyValue(Addr, Pair.Value->AsBool());
			else if (FStrProperty* SP = CastField<FStrProperty>(Prop)) SP->SetPropertyValue(Addr, Pair.Value->AsString());
		}

		// For curve DIs, rebuild the LUT after setting keys
		if (bCurveApplied)
		{
			if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DIInst))
			{
#if WITH_EDITORONLY_DATA
				CurveBase->UpdateLUT();
#endif
			}
		}
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	// Build a descriptive success message
	if (DIConfig.IsValid() && DIConfig->Values.Num() > 0)
	{
		return SuccessStr(FString::Printf(TEXT("DI '%s' set on input '%s'%s"),
			*DIUClass->GetName(), *InputName,
			bCurveConfigApplied ? TEXT(" (curve config applied)") : TEXT(" (config applied, no curve keys matched)")));
	}
	return SuccessStr(FString::Printf(TEXT("DI '%s' set on input '%s'"), *DIUClass->GetName(), *InputName));
}

FMonolithActionResult FMonolithNiagaraActions::CreateScriptFromHLSL(const TSharedPtr<FJsonObject>& Params, ENiagaraScriptUsage Usage)
{
	// === Parse and validate params ===
	FString Name = Params->GetStringField(TEXT("name"));
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	FString HlslBody = Params->GetStringField(TEXT("hlsl"));
	FString Description = Params->HasField(TEXT("description")) ? Params->GetStringField(TEXT("description")) : FString();

	if (Name.IsEmpty()) return FMonolithActionResult::Error(TEXT("'name' is required"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("'save_path' is required"));
	if (HlslBody.IsEmpty()) return FMonolithActionResult::Error(TEXT("'hlsl' is required"));

	// Parse inputs array
	struct FPinDef { FString Name; FNiagaraTypeDefinition Type; };
	TArray<FPinDef> ParsedInputs;
	TArray<FPinDef> ParsedOutputs;

	// Helper: extract an array from a field that may be a JSON array or a string-serialized JSON array
	// (Claude Code sometimes serializes nested JSON arrays as strings via MCP)
	auto GetJsonArray = [](const TSharedPtr<FJsonObject>& P, const FString& FieldName) -> TArray<TSharedPtr<FJsonValue>>
	{
		if (!P->HasField(FieldName)) return {};
		const TArray<TSharedPtr<FJsonValue>>* ArrPtr;
		if (P->TryGetArrayField(FieldName, ArrPtr)) return *ArrPtr;
		// Fallback: try parsing as string
		FString Str = P->GetStringField(FieldName);
		if (!Str.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Parsed;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Str);
			FJsonSerializer::Deserialize(Reader, Parsed);
			return Parsed;
		}
		return {};
	};

	for (const TSharedPtr<FJsonValue>& Val : GetJsonArray(Params, TEXT("inputs")))
	{
		TSharedPtr<FJsonObject> Obj = AsObjectOrParseString(Val);
		if (!Obj.IsValid() || Obj->Values.Num() == 0) continue;
		FString PinName = Obj->GetStringField(TEXT("name"));
		FString TypeStr = Obj->GetStringField(TEXT("type"));
		if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;
		ParsedInputs.Add({ PinName, ResolveNiagaraType(TypeStr) });
	}

	for (const TSharedPtr<FJsonValue>& Val : GetJsonArray(Params, TEXT("outputs")))
	{
		TSharedPtr<FJsonObject> Obj = AsObjectOrParseString(Val);
		if (!Obj.IsValid() || Obj->Values.Num() == 0) continue;
		FString PinName = Obj->GetStringField(TEXT("name"));
		FString TypeStr = Obj->GetStringField(TEXT("type"));
		if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;
		ParsedOutputs.Add({ PinName, ResolveNiagaraType(TypeStr) });
	}

	// Validate: no dots in I/O names — engine generates "In_X" / "Out_X" as HLSL parameter
	// names with bCollapseNamespaces=false, so dots become struct member access → compile error.
	for (const FPinDef& Pin : ParsedInputs)
	{
		if (Pin.Name.Contains(TEXT(".")))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Input name '%s' contains a dot. Use a bare identifier (e.g. 'InColor' not 'Module.InColor'). "
					 "The Niagara translator prefixes 'In_' producing 'In_%s' which is invalid HLSL."),
				*Pin.Name, *Pin.Name));
		}
	}
	for (const FPinDef& Pin : ParsedOutputs)
	{
		if (Pin.Name.Contains(TEXT(".")))
		{
			if (Usage == ENiagaraScriptUsage::Module)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Output name '%s' contains a dot — invalid for module outputs. "
						 "Write to particle attributes directly in the HLSL body (e.g. 'Particles.Color = ...;'). "
						 "The ParameterMap handles namespace resolution. Remove '%s' from outputs."),
					*Pin.Name, *Pin.Name));
			}
			else
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Output name '%s' contains a dot. Use a bare identifier (e.g. 'Result' not 'Module.Result'). "
						 "The translator generates 'Out_%s' which is invalid HLSL."),
					*Pin.Name, *Pin.Name));
			}
		}
	}

	// === Create package and NiagaraScript asset ===
	FString PackagePath, AssetName;
	int32 LastSlash;
	if (!SavePath.FindLastChar('/', LastSlash))
		return FMonolithActionResult::Error(TEXT("Invalid save_path - must contain '/'"));
	PackagePath = SavePath.Left(LastSlash);
	AssetName = SavePath.Mid(LastSlash + 1);

	FString FullPath = PackagePath / AssetName;
	UPackage* Pkg = CreatePackage(*FullPath);
	if (!Pkg) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	UNiagaraScript* Script = NewObject<UNiagaraScript>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!Script) return FMonolithActionResult::Error(TEXT("Failed to create NiagaraScript"));
	Script->Usage = Usage;

	// === Create graph structure manually ===
	// We build the graph ourselves instead of calling InitializeScript because InitializeScript
	// creates ParameterMapGet/Set nodes whose headers are private to NiagaraEditor.
	// Our approach: InputNode → CustomHlslNode (with ParameterMap flow pins via Signature) → OutputNode

	UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script, NAME_None, RF_Transactional);
	UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional);
	Source->NodeGraph = Graph;

	const UEdGraphSchema_Niagara* Schema = Cast<UEdGraphSchema_Niagara>(Graph->GetSchema());
	if (!Schema) Schema = GetDefault<UEdGraphSchema_Niagara>();

	const bool bIsModule = (Usage == ENiagaraScriptUsage::Module);

	// --- OutputNode ---
	FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
	UNiagaraNodeOutput* OutputNode = OutputCreator.CreateNode();
	OutputNode->SetUsage(Usage);
	if (bIsModule)
	{
		// Module outputs use ParameterMap
		OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
	}
	else
	{
		// Function outputs use typed pins — add one per output
		if (ParsedOutputs.Num() > 0)
		{
			OutputNode->Outputs.Add(FNiagaraVariable(ParsedOutputs[0].Type, FName(*ParsedOutputs[0].Name)));
		}
		else
		{
			// Default to float output if none specified
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Output")));
		}
	}
	OutputCreator.Finalize();

	// --- InputNode ---
	FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
	UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
	InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
	if (bIsModule)
	{
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("MapIn"));
	}
	else
	{
		// Function inputs use typed pins — use first input or default float
		if (ParsedInputs.Num() > 0)
		{
			InputNode->Input = FNiagaraVariable(ParsedInputs[0].Type, FName(*ParsedInputs[0].Name));
		}
		else
		{
			InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Input"));
		}
	}
	InputCreator.Finalize();

	// --- Typed UNiagaraNodeInput nodes for each user-defined input ---
	// FindInputNodes (called by UNiagaraNodeFunctionCall::AllocateDefaultPins when
	// HasValidScriptAndGraph() is true) scans ALL graph nodes for UNiagaraNodeInput
	// with Usage == Parameter. These nodes don't need wiring — their mere existence
	// causes the FunctionCall to create override pins that set_module_input_value can write to.
	// ExposureOptions defaults: bExposed=1, bRequired=1 — pins are immediately visible.
	for (int32 i = 0; i < ParsedInputs.Num(); ++i)
	{
		const FPinDef& Input = ParsedInputs[i];

		// For the Function path, the first input already has an InputNode created above.
		// Skip it to avoid a duplicate (FindInputNodes deduplicates by name, but cleaner to not create it).
		if (!bIsModule && i == 0) continue;

		FGraphNodeCreator<UNiagaraNodeInput> TypedInputCreator(*Graph);
		UNiagaraNodeInput* TypedInputNode = TypedInputCreator.CreateNode();
		TypedInputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		TypedInputNode->Input = FNiagaraVariable(Input.Type, FName(*Input.Name));
		TypedInputNode->CallSortPriority = i;  // controls pin order on the FunctionCall node
		TypedInputCreator.Finalize();
	}

	// --- CustomHlsl Node ---
	// Set up the Signature BEFORE Finalize so AllocateDefaultPins creates the correct pins.
	// For Module usage, bRequiresExecPin=true creates ParameterMap flow pins automatically.
	FGraphNodeCreator<UNiagaraNodeCustomHlsl> HlslCreator(*Graph);
	UNiagaraNodeCustomHlsl* HlslNode = HlslCreator.CreateNode(/*bSelectNewNode=*/ false);

	// Set ScriptUsage (public UPROPERTY)
	HlslNode->ScriptUsage = Usage;

	// Set FunctionDisplayName (protected UPROPERTY on UNiagaraNodeFunctionCall — access via reflection)
	{
		FProperty* NameProp = UNiagaraNodeFunctionCall::StaticClass()->FindPropertyByName(TEXT("FunctionDisplayName"));
		if (NameProp)
		{
			void* NameAddr = NameProp->ContainerPtrToValuePtr<void>(HlslNode);
			if (FStrProperty* NameStrProp = CastField<FStrProperty>(NameProp))
			{
				NameStrProp->SetPropertyValue(NameAddr, Name);
			}
		}
	}

	// Set CustomHlsl body via UPROPERTY reflection (it's a private EditAnywhere field)
	// NOTE: No HLSL preprocessing needed — input/output names in the Signature use bare names
	// (e.g. "InValue" not "Module.InValue"). The compiler's ProcessCustomHlsl replaces
	// "InValue" → "In_InValue" and "OutValue" → "Out_OutValue" via ReplaceExactMatchTokens.
	// The Module. namespace aliasing is handled at the system level by CreateAliasedModuleParameterHandle.
	FProperty* HlslProp = UNiagaraNodeCustomHlsl::StaticClass()->FindPropertyByName(TEXT("CustomHlsl"));
	if (HlslProp)
	{
		void* Addr = HlslProp->ContainerPtrToValuePtr<void>(HlslNode);
		FStrProperty* StrProp = CastField<FStrProperty>(HlslProp);
		if (StrProp)
		{
			StrProp->SetPropertyValue(Addr, HlslBody);
		}
	}

	// Build the Signature — this drives pin creation in AllocateDefaultPins.
	// CRITICAL: For Module usage, the ParameterMap must be the FIRST entry in Signature.Inputs
	// and Signature.Outputs (NOT via bRequiresExecPin). BuildParameterMapHistory at
	// NiagaraNodeCustomHlsl.cpp:478 checks: InputPins.Num() == Signature.Inputs.Num() + 1
	// where +1 accounts for the "Add" dynamic pin. If bRequiresExecPin creates extra ParameterMap
	// pins outside the Signature, the count fails and parameter map registration is skipped,
	// causing "Incorrect number of outputs" compile errors.
	HlslNode->Signature.Name = FName(*Name);
	HlslNode->Signature.bRequiresExecPin = false;

	if (bIsModule)
	{
		// Module: ParameterMap as first Signature entry (creates the flow pins),
		// followed by typed I/O for user parameters.
		// IMPORTANT: Use bare names (e.g. "InValue" not "Module.InValue") in the Signature.
		// The Module. namespace causes dots in generated HLSL variable names (In_Module.InValue),
		// which the HLSL compiler parses as struct member access → 'In_Module' undeclared.
		// Module-level parameter aliasing happens at the system level via CreateAliasedModuleParameterHandle.
		HlslNode->Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), FName(TEXT(""))));
		for (const FPinDef& Input : ParsedInputs)
		{
			HlslNode->Signature.Inputs.Add(FNiagaraVariable(Input.Type, FName(*Input.Name)));
		}

		HlslNode->Signature.Outputs.Add(FNiagaraVariableBase(FNiagaraTypeDefinition::GetParameterMapDef(), FName(TEXT(""))));
		for (const FPinDef& Output : ParsedOutputs)
		{
			// Module outputs keep their namespace (e.g. Particles.Position, Module.Result)
			HlslNode->Signature.Outputs.Add(FNiagaraVariableBase(Output.Type, FName(*Output.Name)));
		}
	}
	else
	{
		// Function: no ParameterMap pins, direct typed I/O
		for (const FPinDef& Input : ParsedInputs)
		{
			HlslNode->Signature.Inputs.Add(FNiagaraVariable(Input.Type, FName(*Input.Name)));
		}
		for (const FPinDef& Output : ParsedOutputs)
		{
			HlslNode->Signature.Outputs.Add(FNiagaraVariableBase(Output.Type, FName(*Output.Name)));
		}
	}

	// Finalize calls AllocateDefaultPins which reads Signature to create pins
	HlslCreator.Finalize();

	// === Wire the graph ===
	if (bIsModule)
	{
		// Module wiring: InputNode(MapOut) → HlslNode(MapIn) → OutputNode(MapIn)
		// The ParameterMap pins are unnamed (empty FName) — find them by type
		UEdGraphPin* InputMapOut = InputNode->GetOutputPin(0);
		UEdGraphPin* HlslMapIn = MonolithNiagaraHelpers::GetParameterMapPin(*HlslNode, EGPD_Input);
		UEdGraphPin* HlslMapOut = MonolithNiagaraHelpers::GetParameterMapPin(*HlslNode, EGPD_Output);
		UEdGraphPin* OutputMapIn = OutputNode->GetInputPin(0);

		if (InputMapOut && HlslMapIn)
			Schema->TryCreateConnection(InputMapOut, HlslMapIn);
		if (HlslMapOut && OutputMapIn)
			Schema->TryCreateConnection(HlslMapOut, OutputMapIn);
	}
	else
	{
		// Function wiring: InputNode(typed) → HlslNode(first input) and HlslNode(first output) → OutputNode
		UEdGraphPin* InputOut = InputNode->GetOutputPin(0);
		UEdGraphPin* OutputIn = OutputNode->GetInputPin(0);

		// Connect input node to first typed input pin on HlslNode (skip Add pins and empty-name pins)
		if (InputOut && HlslNode->Pins.Num() > 0)
		{
			for (UEdGraphPin* Pin : HlslNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && !Pin->PinName.IsNone()
					&& Pin->PinName != TEXT("Add"))
				{
					Schema->TryCreateConnection(InputOut, Pin);
					break;
				}
			}
		}

		// Connect first typed output pin on HlslNode to output node (skip Add pins and empty-name pins)
		if (OutputIn)
		{
			for (UEdGraphPin* Pin : HlslNode->Pins)
			{
				if (Pin->Direction == EGPD_Output && !Pin->PinName.IsNone()
					&& Pin->PinName != TEXT("Add"))
				{
					Schema->TryCreateConnection(Pin, OutputIn);
					break;
				}
			}
		}
	}

	// Mark graph as needing recompile
	HlslNode->MarkNodeRequiresSynchronization(TEXT("MonolithHLSL"), true);

	// Set source on script — compilation happens when the module is added to a system
	Script->SetLatestSource(Source);

	// === Register and save ===
	FAssetRegistryModule::AssetCreated(Script);
	Pkg->MarkPackageDirty();

	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(Pkg, Script, *PackageFilename, SaveArgs);
	}

	// === Build response ===
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Script->GetPathName());

	int32 NodeCount = Graph->Nodes.Num();
	Result->SetNumberField(TEXT("node_count"), NodeCount);

	TArray<TSharedPtr<FJsonValue>> InputPinNames, OutputPinNames;
	for (const FPinDef& P : ParsedInputs)
		InputPinNames.Add(MakeShared<FJsonValueString>(P.Name));
	for (const FPinDef& P : ParsedOutputs)
		OutputPinNames.Add(MakeShared<FJsonValueString>(P.Name));
	Result->SetArrayField(TEXT("input_pins"), InputPinNames);
	Result->SetArrayField(TEXT("output_pins"), OutputPinNames);

	return SuccessObj(Result);
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateModuleFromHLSL(const TSharedPtr<FJsonObject>& Params)
{
	return CreateScriptFromHLSL(Params, ENiagaraScriptUsage::Module);
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateFunctionFromHLSL(const TSharedPtr<FJsonObject>& Params)
{
	return CreateScriptFromHLSL(Params, ENiagaraScriptUsage::Function);
}

// ============================================================================
// Parameter Actions (9)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetAllParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FString EmitterFilter = Params->HasField(TEXT("emitter")) ? Params->GetStringField(TEXT("emitter")) : TEXT("");
	FString ScopeFilter = Params->HasField(TEXT("scope")) ? Params->GetStringField(TEXT("scope")) : TEXT("");

	TArray<TSharedPtr<FJsonValue>> All;

	// Phase 1: User parameters (system-level)
	if (EmitterFilter.IsEmpty() && (ScopeFilter.IsEmpty() || ScopeFilter.Equals(TEXT("User"), ESearchCase::IgnoreCase)))
	{
		FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
		CollectParametersFromStore(US, TEXT("User"), All);
	}

	// Phase 2: Emitter parameters
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (const FNiagaraEmitterHandle& H : Handles)
	{
		FString EmitterName = H.GetName().ToString();
		if (!EmitterFilter.IsEmpty() && EmitterName != EmitterFilter) continue;

		FVersionedNiagaraEmitterData* ED = H.GetEmitterData();
		if (!ED) continue;
		FString EScope = FString::Printf(TEXT("Emitter.%s"), *EmitterName);
		static const ENiagaraScriptUsage Usages[] = {
			ENiagaraScriptUsage::EmitterSpawnScript, ENiagaraScriptUsage::EmitterUpdateScript,
			ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript,
		};
		for (ENiagaraScriptUsage U : Usages)
		{
			FString UStr = StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(U));
			FString FullScope = FString::Printf(TEXT("%s.%s"), *EScope, *UStr);
			if (!ScopeFilter.IsEmpty() && !FullScope.Contains(ScopeFilter)) continue;

			UNiagaraScript* S = ED->GetScript(U, FGuid());
			if (!S) continue;
			// UE 5.7 FIX: direct UPROPERTY access, no getter
			const FNiagaraParameterStore& PS = S->RapidIterationParameters;
			CollectParametersFromStore(PS, FullScope, All);
		}
	}
	return SuccessStr(JsonArrayToString(All));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetUserParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Use ReadParameterVariables (live store entries) instead of GetUserParameters()
	// which the engine warns returns STALE redirect-map keys.
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<TSharedPtr<FJsonValue>> Arr;
	CollectParametersFromStore(US, TEXT("User"), Arr);
	return SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetParameterValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("parameter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);

	FString Search = ParamName;
	if (Search.StartsWith(TEXT("User."))) Search = Search.Mid(5); // Strip "User." prefix — store names are unprefixed

	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString().Equals(Search, ESearchCase::IgnoreCase))
		{
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("name"), P.GetName().ToString());
			R->SetStringField(TEXT("type"), P.GetType().GetName());
			R->SetStringField(TEXT("value"), SerializeParameterValue(P, US));
			return SuccessObj(R);
		}
	}
	return FMonolithActionResult::Error(TEXT("Parameter not found"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetParameterType(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeName = Params->GetStringField(TEXT("type"));
	FNiagaraTypeDefinition TD = ResolveNiagaraType(TypeName);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("name"), TD.GetName());
	R->SetNumberField(TEXT("size"), TD.GetSize());
	R->SetBoolField(TEXT("is_float_primitive"), TD == FNiagaraTypeDefinition::GetFloatDef());
	R->SetBoolField(TEXT("is_data_interface"), TD.IsDataInterface());
	R->SetBoolField(TEXT("is_enum"), TD.IsEnum());
	R->SetBoolField(TEXT("is_valid"), TD.IsValid());
	if (TD.GetStruct()) R->SetStringField(TEXT("struct_name"), TD.GetStruct()->GetName());

	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleTraceParameterBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("parameter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FString Search = ParamName;
	if (Search.StartsWith(TEXT("User."))) Search = Search.Mid(5); // Strip "User." prefix — store names are unprefixed
	FString PrefixedSearch = TEXT("User.") + Search; // Keep prefixed form for graph pin matching

	TSharedRef<FJsonObject> Trace = MakeShared<FJsonObject>();
	Trace->SetStringField(TEXT("parameter"), PrefixedSearch);

	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);

	bool bFound = false;
	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString() == Search)
		{
			bFound = true;
			Trace->SetStringField(TEXT("type"), P.GetType().GetName());
			Trace->SetStringField(TEXT("source"), TEXT("ExposedParameters"));
			Trace->SetStringField(TEXT("value"), SerializeParameterValue(P, US));
			break;
		}
	}
	if (!bFound)
	{
		Trace->SetStringField(TEXT("error"), TEXT("Parameter not found"));
		return SuccessObj(Trace);
	}

	TArray<TSharedPtr<FJsonValue>> Bindings;
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	static const ENiagaraScriptUsage AllUsages[] = {
		ENiagaraScriptUsage::EmitterSpawnScript, ENiagaraScriptUsage::EmitterUpdateScript,
		ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript,
	};
	for (const FNiagaraEmitterHandle& H : Handles)
	{
		FString EN = H.GetName().ToString();
		for (ENiagaraScriptUsage U : AllUsages)
		{
			UNiagaraNodeOutput* Out = FindOutputNode(System, H.GetId().ToString(), U);
			if (!Out) continue;
			TArray<UNiagaraNodeFunctionCall*> Mods;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
			for (UNiagaraNodeFunctionCall* MN : Mods)
			{
				if (!MN) continue;
				for (UEdGraphPin* Pin : MN->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					for (UEdGraphPin* LP : Pin->LinkedTo)
					{
						FString LN = LP->PinName.ToString();
						if (LN.Contains(PrefixedSearch) || LN.Contains(Search))
						{
							TSharedRef<FJsonObject> BO = MakeShared<FJsonObject>();
							BO->SetStringField(TEXT("emitter"), EN);
							BO->SetStringField(TEXT("module"), MN->GetFunctionName());
							BO->SetStringField(TEXT("input_pin"), Pin->PinName.ToString());
							BO->SetStringField(TEXT("usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(U)));
							Bindings.Add(MakeShared<FJsonValueObject>(BO));
						}
					}
				}
			}
		}
	}
	Trace->SetArrayField(TEXT("bindings"), Bindings);
	return SuccessObj(Trace);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddUserParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	// Accept "name" (canonical) or "parameter_name" (common alias)
	FString ParamName = Params->HasField(TEXT("name")) ? Params->GetStringField(TEXT("name")) : Params->GetStringField(TEXT("parameter_name"));
	FString TypeName = Params->GetStringField(TEXT("type"));
	// Accept "default" (canonical) or "default_value" (common alias)
	TSharedPtr<FJsonValue> DefaultJV = Params->HasField(TEXT("default")) ? Params->TryGetField(TEXT("default"))
		: Params->HasField(TEXT("default_value")) ? Params->TryGetField(TEXT("default_value")) : TSharedPtr<FJsonValue>();

	if (ParamName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Parameter name is required — pass as \"name\" field"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FNiagaraTypeDefinition TD = ResolveNiagaraType(TypeName);
	FNiagaraVariable NV = MakeUserVariable(ParamName, TD);

	// Pre-set the default value on the FNiagaraVariable BEFORE adding to the store.
	// Setting after AddParameter fails because FNiagaraUserRedirectionParameterStore
	// has a redirection layer — the name lookup in SetParameterValue may not match.
	// By setting data on the variable first, AddParameter copies it into the store directly.
	bool bDefaultSet = false;
	if (DefaultJV.IsValid())
	{
		NV.AllocateData();
		if (TD == FNiagaraTypeDefinition::GetColorDef())
		{
			TSharedPtr<FJsonObject> O = AsObjectOrParseString(DefaultJV);
			if (O.IsValid())
			{
				FLinearColor V(
					static_cast<float>(O->GetNumberField(TEXT("r"))),
					static_cast<float>(O->GetNumberField(TEXT("g"))),
					static_cast<float>(O->GetNumberField(TEXT("b"))),
					O->HasField(TEXT("a")) ? static_cast<float>(O->GetNumberField(TEXT("a"))) : 1.0f);
				NV.SetValue<FLinearColor>(V);
				bDefaultSet = true;
			}
		}
		else if (TD == FNiagaraTypeDefinition::GetFloatDef())
		{
			NV.SetValue<float>(static_cast<float>(DefaultJV->AsNumber()));
			bDefaultSet = true;
		}
		else if (TD == FNiagaraTypeDefinition::GetIntDef())
		{
			NV.SetValue<int32>(static_cast<int32>(DefaultJV->AsNumber()));
			bDefaultSet = true;
		}
		else if (TD == FNiagaraTypeDefinition::GetBoolDef())
		{
			FNiagaraBool BV; BV.SetValue(DefaultJV->AsBool());
			NV.SetValue<FNiagaraBool>(BV);
			bDefaultSet = true;
		}
		else if (TD == FNiagaraTypeDefinition::GetVec3Def() || TD == FNiagaraTypeDefinition::GetPositionDef())
		{
			TSharedPtr<FJsonObject> O = AsObjectOrParseString(DefaultJV);
			if (O.IsValid())
			{
				FVector3f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
					static_cast<float>(O->GetNumberField(TEXT("y"))),
					static_cast<float>(O->GetNumberField(TEXT("z"))));
				NV.SetValue<FVector3f>(V);
				bDefaultSet = true;
			}
		}
		else if (TD == FNiagaraTypeDefinition::GetVec2Def())
		{
			TSharedPtr<FJsonObject> O = AsObjectOrParseString(DefaultJV);
			if (O.IsValid())
			{
				FVector2f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
					static_cast<float>(O->GetNumberField(TEXT("y"))));
				NV.SetValue<FVector2f>(V);
				bDefaultSet = true;
			}
		}
		else if (TD == FNiagaraTypeDefinition::GetVec4Def() || TD == FNiagaraTypeDefinition::GetQuatDef())
		{
			TSharedPtr<FJsonObject> O = AsObjectOrParseString(DefaultJV);
			if (O.IsValid())
			{
				FVector4f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
					static_cast<float>(O->GetNumberField(TEXT("y"))),
					static_cast<float>(O->GetNumberField(TEXT("z"))),
					static_cast<float>(O->GetNumberField(TEXT("w"))));
				NV.SetValue<FVector4f>(V);
				bDefaultSet = true;
			}
		}
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddUP", "Add User Parameter"));
	System->Modify();
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();

	// Let the entire add + transaction cascade happen normally.
	// AddParameter fires OnStructureChangedDelegate, EndTransaction fires OnChangedDelegate —
	// both trigger the editor's FNiagaraSystemUserParameterBuilder::Rebuild() which resets
	// the value to (0,0,0,1). We can't prevent this without breaking editor sync.
	//
	// Strategy: let the cascade finish, THEN write our value in a suppressed scope.
	// The suppression prevents our write from triggering another cascade.
	US.AddParameter(NV, true, true);
	GEditor->EndTransaction();

	// NOW the slot exists with the editor's default (0,0,0,1). All cascades have settled.
	// Write our actual value to BOTH storage layers:
	// Layer 1: Runtime store (ExposedParameters) — for immediate use
	// Layer 2: Editor data (UNiagaraScriptVariable::DefaultValueVariant) — survives recompile/re-sync
	if (bDefaultSet)
	{
		// Layer 1: runtime store — use SetParameterValue<T> which handles the User
		// redirection layer correctly (raw IndexOf + SetParameterData fails because
		// GetUserParameters() returns bare names but the store indexes by "User.X" form)
		{
			FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(US);
			// Re-find the variable in the store after AddParameter — the store
			// now contains it under the redirected name
			TArray<FNiagaraVariable> UP;
			US.GetUserParameters(UP);
			FNiagaraVariable StoreVar;
			bool bFoundInStore = false;
			FString SearchName = ParamName;
			if (SearchName.StartsWith(TEXT("User."))) SearchName = SearchName.Mid(5);
			for (const FNiagaraVariable& P : UP)
			{
				if (P.GetName().ToString().Equals(SearchName, ESearchCase::IgnoreCase))
				{
					StoreVar = P;
					bFoundInStore = true;
					break;
				}
			}
			if (bFoundInStore)
			{
				if (TD == FNiagaraTypeDefinition::GetColorDef())
				{
					US.SetParameterValue<FLinearColor>(NV.GetValue<FLinearColor>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetFloatDef())
				{
					US.SetParameterValue<float>(NV.GetValue<float>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetIntDef())
				{
					US.SetParameterValue<int32>(NV.GetValue<int32>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetBoolDef())
				{
					US.SetParameterValue<FNiagaraBool>(NV.GetValue<FNiagaraBool>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetVec2Def())
				{
					US.SetParameterValue<FVector2f>(NV.GetValue<FVector2f>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetVec3Def() || TD == FNiagaraTypeDefinition::GetPositionDef())
				{
					US.SetParameterValue<FVector3f>(NV.GetValue<FVector3f>(), StoreVar, true);
				}
				else if (TD == FNiagaraTypeDefinition::GetVec4Def() || TD == FNiagaraTypeDefinition::GetQuatDef())
				{
					US.SetParameterValue<FVector4f>(NV.GetValue<FVector4f>(), StoreVar, true);
				}
			}
		}

		// Layer 2: editor data — persists the default across recompiles
		if (UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData()))
		{
			UNiagaraScriptVariable* ScriptVar = EditorData->FindOrAddUserScriptVariable(NV, *System);
			if (ScriptVar)
			{
				ScriptVar->Modify();
				ScriptVar->DefaultMode = ENiagaraDefaultMode::Value;
				ScriptVar->SetDefaultValueData(NV.GetData());
			}
		}
	}

	if (DefaultJV.IsValid() && !bDefaultSet)
	{
		return SuccessStr(FString::Printf(TEXT("Added user parameter '%s' but failed to set default value — check value format matches type '%s'"), *ParamName, *TypeName));
	}
	return SuccessStr(FString::Printf(TEXT("Added user parameter '%s'%s"), *ParamName, bDefaultSet ? TEXT(" with default") : TEXT("")));
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveUserParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FString Search = ParamName;
	if (Search.StartsWith(TEXT("User."))) Search = Search.Mid(5); // Strip "User." prefix — store names are unprefixed

	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);

	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString().Equals(Search, ESearchCase::IgnoreCase))
		{
			GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemUP", "Remove User Parameter"));
			System->Modify();
			US.RemoveParameter(P);
			GEditor->EndTransaction();
			return SuccessStr(FString::Printf(TEXT("Removed parameter '%s'"), *ParamName));
		}
	}
	return FMonolithActionResult::Error(TEXT("Parameter not found"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetParameterDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("parameter"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FString Search = ParamName;
	if (Search.StartsWith(TEXT("User."))) Search = Search.Mid(5); // Strip "User." prefix — store names are unprefixed

	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);

	FNiagaraVariable Found;
	bool bFound = false;
	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString().Equals(Search, ESearchCase::IgnoreCase))
		{ Found = P; bFound = true; break; }
	}
	if (!bFound) return FMonolithActionResult::Error(TEXT("Parameter not found"));

	// Let the transaction complete and all editor cascades settle first.
	// Then write the value in a suppressed scope so it sticks.
	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetPDef", "Set Parameter Default"));
	System->Modify();
	GEditor->EndTransaction();

	FNiagaraTypeDefinition FTD = Found.GetType();
	bool bOk = false;

	// Layer 1: runtime store — SetTypedParameterValue uses SetParameterValue<T> which
	// handles the User redirection layer correctly (unlike raw IndexOf + SetParameterData
	// which fails because GetUserParameters() returns bare names but the store indexes by
	// the redirected "User.X" form).
	{
		FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(US);
		bOk = SetTypedParameterValue(US, Found, FTD, JV);
	}

	// Layer 2: editor data — persists across recompiles
	if (bOk)
	{
		if (UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData()))
		{
			// Found has the short name from GetUserParameters — need full User. name for FindOrAddUserScriptVariable
			FNiagaraVariable FullVar = Found;
			if (!FullVar.GetName().ToString().StartsWith(TEXT("User.")))
			{
				FString FullName = TEXT("User.") + FullVar.GetName().ToString();
				FullVar.SetName(FName(*FullName));
			}
			// Allocate data and copy from the store so we have the correct value
			FNiagaraVariable WriteVar = Found;
			WriteVar.AllocateData();
			if (FTD == FNiagaraTypeDefinition::GetColorDef())
			{
				TSharedPtr<FJsonObject> O = AsObjectOrParseString(JV);
				if (O.IsValid())
				{
					FLinearColor V(
						static_cast<float>(O->GetNumberField(TEXT("r"))),
						static_cast<float>(O->GetNumberField(TEXT("g"))),
						static_cast<float>(O->GetNumberField(TEXT("b"))),
						O->HasField(TEXT("a")) ? static_cast<float>(O->GetNumberField(TEXT("a"))) : 1.0f);
					WriteVar.SetValue<FLinearColor>(V);
				}
			}
			else if (FTD == FNiagaraTypeDefinition::GetFloatDef())
			{
				WriteVar.SetValue<float>(static_cast<float>(JV->AsNumber()));
			}
			else if (FTD == FNiagaraTypeDefinition::GetIntDef())
			{
				WriteVar.SetValue<int32>(static_cast<int32>(JV->AsNumber()));
			}
			else if (FTD == FNiagaraTypeDefinition::GetBoolDef())
			{
				FNiagaraBool BV; BV.SetValue(JV->AsBool());
				WriteVar.SetValue<FNiagaraBool>(BV);
			}
			else if (FTD == FNiagaraTypeDefinition::GetVec3Def() || FTD == FNiagaraTypeDefinition::GetPositionDef())
			{
				TSharedPtr<FJsonObject> O = AsObjectOrParseString(JV);
				if (O.IsValid())
				{
					FVector3f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
						static_cast<float>(O->GetNumberField(TEXT("y"))),
						static_cast<float>(O->GetNumberField(TEXT("z"))));
					WriteVar.SetValue<FVector3f>(V);
				}
			}
			else if (FTD == FNiagaraTypeDefinition::GetVec2Def())
			{
				TSharedPtr<FJsonObject> O = AsObjectOrParseString(JV);
				if (O.IsValid())
				{
					FVector2f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
						static_cast<float>(O->GetNumberField(TEXT("y"))));
					WriteVar.SetValue<FVector2f>(V);
				}
			}
			else if (FTD == FNiagaraTypeDefinition::GetVec4Def() || FTD == FNiagaraTypeDefinition::GetQuatDef())
			{
				TSharedPtr<FJsonObject> O = AsObjectOrParseString(JV);
				if (O.IsValid())
				{
					FVector4f V(static_cast<float>(O->GetNumberField(TEXT("x"))),
						static_cast<float>(O->GetNumberField(TEXT("y"))),
						static_cast<float>(O->GetNumberField(TEXT("z"))),
						static_cast<float>(O->GetNumberField(TEXT("w"))));
					WriteVar.SetValue<FVector4f>(V);
				}
			}
			if (WriteVar.IsDataAllocated())
			{
				UNiagaraScriptVariable* ScriptVar = EditorData->FindOrAddUserScriptVariable(FullVar, *System);
				if (ScriptVar)
				{
					ScriptVar->Modify();
					ScriptVar->DefaultMode = ENiagaraDefaultMode::Value;
					ScriptVar->SetDefaultValueData(WriteVar.GetData());
				}
			}
		}
	}

	return bOk ? SuccessStr(TEXT("Default set")) : FMonolithActionResult::Error(TEXT("Unsupported type"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetCurveValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleName = Params->GetStringField(TEXT("module_node"));
	if (ModuleName.IsEmpty()) ModuleName = Params->GetStringField(TEXT("module"));
	if (ModuleName.IsEmpty()) ModuleName = Params->GetStringField(TEXT("module_name"));
	FString InputName = Params->GetStringField(TEXT("input"));
	if (InputName.IsEmpty()) InputName = Params->GetStringField(TEXT("input_name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Bug 1 fix: "keys" arrives as a parsed JSON array — don't serialize to string then re-parse.
	TSharedPtr<FJsonValue> KeysField = Params->TryGetField(TEXT("keys"));
	if (!KeysField.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: keys"));
	TArray<TSharedPtr<FJsonValue>> Keys;
	if (KeysField->Type == EJson::Array)
	{
		Keys = KeysField->AsArray();
	}
	else if (KeysField->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(KeysField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, Keys))
			return FMonolithActionResult::Error(TEXT("Failed to parse keys string"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'keys' must be an array"));
	}

	TArray<FString> KS;
	for (const TSharedPtr<FJsonValue>& KV : Keys)
	{
		TSharedPtr<FJsonObject> KO = KV->AsObject();
		if (!KO) continue;
		float T = static_cast<float>(KO->GetNumberField(TEXT("time")));
		float V = static_cast<float>(KO->GetNumberField(TEXT("value")));
		float AT = KO->HasField(TEXT("arrive_tangent")) ? static_cast<float>(KO->GetNumberField(TEXT("arrive_tangent"))) : 0.f;
		float LT = KO->HasField(TEXT("leave_tangent")) ? static_cast<float>(KO->GetNumberField(TEXT("leave_tangent"))) : 0.f;
		KS.Add(FString::Printf(TEXT("(Time=%f,Value=%f,ArriveTangent=%f,LeaveTangent=%f)"), T, V, AT, LT));
	}
	FString CurveStr = TEXT("(") + FString::Join(KS, TEXT(",")) + TEXT(")");

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found. Use list_emitters to get valid emitter names or GUIDs."), *EmitterHandleId));

	// Bug fix: use the engine's full GetStackFunctionInputs API (same as get_module_inputs)
	// instead of the local pin-scan helper, which returns display names that don't match
	// the short names callers get from get_module_inputs.
	ENiagaraScriptUsage CurveFoundUsage = ENiagaraScriptUsage::ParticleUpdateScript;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleName, &CurveFoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module not found"));
	TArray<FNiagaraVariable> Inputs;
	int32 CurveEmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (CurveEmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[CurveEmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, CurveFoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, CurveFoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	FNiagaraTypeDefinition InputType;
	bool bInputFound = false;
	FName MatchedFullName; // store the full Module.X name for correct PH construction
	FName InputFName(*InputName);
	FString CurveInputNoSpaces = InputName;
	CurveInputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	for (const FNiagaraVariable& In : Inputs)
	{
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		bool bMatch = (ShortName == InputFName || In.GetName() == InputFName);
		// Fallback: space-stripped comparison (e.g. "ScaleAlpha" matches "Scale Alpha")
		if (!bMatch)
		{
			FString ShortNameNoSpaces = ShortName.ToString();
			ShortNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = ShortNameNoSpaces.Equals(CurveInputNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch)
		{
			InputType = In.GetType();
			MatchedFullName = In.GetName();
			bInputFound = true;
			break;
		}
	}

	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : Inputs) { ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString()); }
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' not found on module '%s'. Valid inputs: [%s]"),
			*InputName, *ModuleName, *FString::Join(ValidNames, TEXT(", "))));
	}

	// Guard: DI-typed inputs cannot have string DefaultValues — they need set_module_input_di
	if (InputType.IsDataInterface())
	{
		// Extract the DI class name for a helpful error message
		FString DIClassName = InputType.GetClass() ? InputType.GetClass()->GetName() : InputType.GetName();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Input '%s' is a DataInterface type. Use set_module_input_di with di_class: \"%s\" instead. Example: {\"op\": \"set_module_input_di\", \"input\": \"%s\", \"di_class\": \"%s\", \"config\": {\"keys\": [...]}}"),
			*InputName, *DIClassName, *InputName, *DIClassName));
	}

	// Use the full Module.X name from the engine enumeration for correct namespace aliasing
	FNiagaraParameterHandle PH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedFullName), MN);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetCurve", "Set Curve"));
	System->Modify();

	// UE 5.7 FIX: 5-param version
	UEdGraphPin& OP = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*MN, PH, InputType, FGuid(), FGuid());

	// Guard: break existing links so the literal DefaultValue takes effect
	if (OP.LinkedTo.Num() > 0)
	{
		OP.BreakAllPinLinks();
	}

	OP.DefaultValue = CurveStr;

	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(FString::Printf(TEXT("Set curve with %d keys"), Keys.Num()));
}

// ============================================================================
// Renderer Actions (6)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString RendererClassStr = Params->GetStringField(TEXT("class"));
	if (RendererClassStr.IsEmpty()) RendererClassStr = Params->GetStringField(TEXT("renderer_class"));
	if (RendererClassStr.IsEmpty()) RendererClassStr = Params->GetStringField(TEXT("renderer_type"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UClass* RC = ResolveRendererClass(RendererClassStr);
	if (!RC) return FMonolithActionResult::Error(TEXT("Unknown renderer class"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddRend", "Add Renderer"));
	System->Modify();

	FVersionedNiagaraEmitter EI = Handle.GetInstance();
	UNiagaraRendererProperties* NR = NewObject<UNiagaraRendererProperties>(EI.Emitter, RC, NAME_None, RF_Transactional);
	if (!NR) { GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("Failed to create renderer")); }

	EI.Emitter->AddRenderer(NR, EI.Version);
	GEditor->EndTransaction();
	System->RequestCompile(false);

	int32 NewIdx = ED->GetRenderers().Num() - 1;
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("renderer_index"), NewIdx);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FVersionedNiagaraEmitterData* ED = nullptr;
	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex, &ED);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitter EI = Handle.GetInstance();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemRend", "Remove Renderer"));
	System->Modify();
	EI.Emitter->RemoveRenderer(Rend, EI.Version);
	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(TEXT("Renderer removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString MaterialPath = Params->GetStringField(TEXT("material"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Mat) return FMonolithActionResult::Error(TEXT("Failed to load material"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRendMat", "Set Renderer Material"));
	System->Modify();
	Rend->Modify();
	bool bOk = false;

	if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend))
	{ S->Material = Mat; bOk = true; }
	else if (UNiagaraMeshRendererProperties* M = Cast<UNiagaraMeshRendererProperties>(Rend))
	{ M->bOverrideMaterials = true; M->OverrideMaterials.SetNum(1); M->OverrideMaterials[0].ExplicitMat = Mat; bOk = true; }
	else if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend))
	{ Rib->Material = Mat; bOk = true; }
	else
	{
		FProperty* MP = Rend->GetClass()->FindPropertyByName(TEXT("Material"));
		if (FObjectProperty* OP = CastField<FObjectProperty>(MP))
		{ OP->SetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(Rend), Mat); bOk = true; }
	}

	GEditor->EndTransaction();
	if (bOk) System->RequestCompile(false);
	return bOk ? SuccessStr(TEXT("Material set")) : FMonolithActionResult::Error(TEXT("Unsupported renderer type"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString PropertyName = Params->GetStringField(TEXT("property"));
	if (PropertyName.IsEmpty()) PropertyName = Params->GetStringField(TEXT("property_name"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	FProperty* Prop = Rend->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop) return FMonolithActionResult::Error(FString::Printf(TEXT("Property '%s' not found"), *PropertyName));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRendProp", "Set Renderer Property"));
	System->Modify();
	Rend->Modify();

	void* Addr = Prop->ContainerPtrToValuePtr<void>(Rend);
	bool bOk = false;

	if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) { FP->SetPropertyValue(Addr, static_cast<float>(JV->AsNumber())); bOk = true; }
	else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) { DP->SetPropertyValue(Addr, JV->AsNumber()); bOk = true; }
	else if (FIntProperty* IP = CastField<FIntProperty>(Prop)) { IP->SetPropertyValue(Addr, static_cast<int32>(JV->AsNumber())); bOk = true; }
	else if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) { BP->SetPropertyValue(Addr, JV->AsBool()); bOk = true; }
	else if (FStrProperty* SP = CastField<FStrProperty>(Prop)) { SP->SetPropertyValue(Addr, JV->AsString()); bOk = true; }
	else if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
	{
		UEnum* E = EP->GetEnum();
		if (E)
		{
			int64 EV = E->GetValueByNameString(JV->AsString());
			if (EV == INDEX_NONE) EV = static_cast<int64>(JV->AsNumber());
			FNumericProperty* UP2 = EP->GetUnderlyingProperty();
			if (UP2) { UP2->SetIntPropertyValue(Addr, EV); bOk = true; }
		}
	}
	else if (FByteProperty* ByP = CastField<FByteProperty>(Prop))
	{
		if (ByP->Enum)
		{
			int64 EV = ByP->Enum->GetValueByNameString(JV->AsString());
			if (EV == INDEX_NONE) EV = static_cast<int64>(JV->AsNumber());
			ByP->SetPropertyValue(Addr, static_cast<uint8>(EV));
		}
		else ByP->SetPropertyValue(Addr, static_cast<uint8>(JV->AsNumber()));
		bOk = true;
	}
	else if (FObjectProperty* OP = CastField<FObjectProperty>(Prop))
	{
		UObject* Obj = LoadObject<UObject>(nullptr, *JV->AsString());
		if (Obj) { OP->SetObjectPropertyValue(Addr, Obj); bOk = true; }
	}
	else
	{
		bOk = Prop->ImportText_Direct(*JV->AsString(), Addr, Rend, PPF_None) != nullptr;
	}

	GEditor->EndTransaction();
	if (bOk) System->RequestCompile(false);
	return bOk ? SuccessStr(TEXT("Property set")) : FMonolithActionResult::Error(TEXT("Failed to set property"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetRendererBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	TArray<TSharedPtr<FJsonValue>> Arr;
	UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
	for (TFieldIterator<FProperty> It(Rend->GetClass()); It; ++It)
	{
		FStructProperty* SP = CastField<FStructProperty>(*It);
		if (!SP || SP->Struct != BindingStruct) continue;

		const FNiagaraVariableAttributeBinding* Binding =
			SP->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Rend);

		TSharedRef<FJsonObject> BO = MakeShared<FJsonObject>();
		BO->SetStringField(TEXT("name"), (*It)->GetName());

		const FNiagaraVariableBase& BV = Binding->GetParamMapBindableVariable();
		FString BoundName = BV.GetName().ToString();
		BO->SetStringField(TEXT("bound_to"), BoundName.IsEmpty() || BoundName == TEXT("NONE") ? TEXT("(unbound)") : BoundName);
		BO->SetStringField(TEXT("type"), BV.GetType().GetName());

		Arr.Add(MakeShared<FJsonValueObject>(BO));
	}
	return SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString BindingName = Params->GetStringField(TEXT("binding_name"));
	FString AttributePath = Params->GetStringField(TEXT("attribute"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	FStructProperty* BP = nullptr;
	for (TFieldIterator<FProperty> It(Rend->GetClass()); It; ++It)
	{
		if ((*It)->GetName() == BindingName) { BP = CastField<FStructProperty>(*It); break; }
	}
	if (!BP) return FMonolithActionResult::Error(TEXT("Binding property not found"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRendBind", "Set Renderer Binding"));
	System->Modify();
	Rend->Modify();

	void* Addr = BP->ContainerPtrToValuePtr<void>(Rend);
	FString ImportText = FString::Printf(TEXT("(BoundVariable=(Name=\"%s\"))"), *AttributePath);
	bool bOk = BP->ImportText_Direct(*ImportText, Addr, Rend, PPF_None) != nullptr;
	if (!bOk)
	{
		FString Fallback = FString::Printf(TEXT("(BoundVariable=(Name=\"%s\",TypeDefHandle=(RegisteredTypeIndex=-1)))"), *AttributePath);
		bOk = BP->ImportText_Direct(*Fallback, Addr, Rend, PPF_None) != nullptr;
	}

	GEditor->EndTransaction();
	if (bOk) System->RequestCompile(false);
	return bOk ? SuccessStr(TEXT("Binding set")) : FMonolithActionResult::Error(TEXT("Failed to set binding"));
}

// ============================================================================
// Batch Actions (2)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Bug 1 fix: "operations" arrives as a parsed JSON array — don't serialize to string then re-parse.
	// TryGetField returns the array value directly; if it was sent as a pre-serialized string we fall back.
	TArray<TSharedPtr<FJsonValue>> Ops;
	TSharedPtr<FJsonValue> OpsField = Params->TryGetField(TEXT("operations"));
	if (!OpsField.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: operations"));
	if (OpsField->Type == EJson::Array)
	{
		Ops = OpsField->AsArray();
	}
	else if (OpsField->Type == EJson::String)
	{
		// Fallback: caller pre-serialized the array to a string
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OpsField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, Ops))
			return FMonolithActionResult::Error(TEXT("Failed to parse operations string"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'operations' must be an array"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "BatchExec", "Batch Execute"));
	System->Modify();

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Ok = 0, Fail = 0;

	for (int32 i = 0; i < Ops.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Op = Ops[i]->AsObject();
		if (!Op.IsValid()) { Fail++; continue; }

		FString OpName = Op->GetStringField(TEXT("op"));
		TSharedRef<FJsonObject> RO = MakeShared<FJsonObject>();
		RO->SetNumberField(TEXT("index"), i);
		RO->SetStringField(TEXT("op"), OpName);

		// Delegate to individual handlers by constructing param objects
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("system_path"), SystemPath);

		// Copy all fields from Op to SubParams
		for (auto& Pair : Op->Values)
		{
			SubParams->SetField(Pair.Key, Pair.Value);
		}

		FMonolithActionResult SubResult = FMonolithActionResult::Error(TEXT("Unknown op"));

		if (OpName == TEXT("add_emitter")) SubResult = HandleAddEmitter(SubParams);
		else if (OpName == TEXT("remove_emitter")) SubResult = HandleRemoveEmitter(SubParams);
		else if (OpName == TEXT("add_module")) SubResult = HandleAddModule(SubParams);
		else if (OpName == TEXT("remove_module")) SubResult = HandleRemoveModule(SubParams);
		else if (OpName == TEXT("set_module_input_value") || OpName == TEXT("set_module_input")) SubResult = HandleSetModuleInputValue(SubParams);
		else if (OpName == TEXT("set_module_input_binding") || OpName == TEXT("set_module_binding")) SubResult = HandleSetModuleInputBinding(SubParams);
		else if (OpName == TEXT("set_emitter_property")) SubResult = HandleSetEmitterProperty(SubParams);
		else if (OpName == TEXT("add_renderer")) SubResult = HandleAddRenderer(SubParams);
		else if (OpName == TEXT("remove_renderer")) SubResult = HandleRemoveRenderer(SubParams);
		else if (OpName == TEXT("set_renderer_material")) SubResult = HandleSetRendererMaterial(SubParams);
		else if (OpName == TEXT("set_renderer_property")) SubResult = HandleSetRendererProperty(SubParams);
		else if (OpName == TEXT("add_user_parameter") || OpName == TEXT("add_user_param")) SubResult = HandleAddUserParameter(SubParams);
		else if (OpName == TEXT("remove_user_parameter") || OpName == TEXT("remove_user_param")) SubResult = HandleRemoveUserParameter(SubParams);
		else if (OpName == TEXT("set_parameter_default")) SubResult = HandleSetParameterDefault(SubParams);
		else if (OpName == TEXT("set_module_enabled")) SubResult = HandleSetModuleEnabled(SubParams);
		else if (OpName == TEXT("set_module_input_di")) SubResult = HandleSetModuleInputDI(SubParams);
		else if (OpName == TEXT("set_curve_value")) SubResult = HandleSetCurveValue(SubParams);
		else if (OpName == TEXT("move_module")) SubResult = HandleMoveModule(SubParams);
		else if (OpName == TEXT("set_emitter_enabled")) SubResult = HandleSetEmitterEnabled(SubParams);
		else if (OpName == TEXT("reorder_emitters")) SubResult = HandleReorderEmitters(SubParams);
		else if (OpName == TEXT("duplicate_emitter")) SubResult = HandleDuplicateEmitter(SubParams);
		else if (OpName == TEXT("set_renderer_binding")) SubResult = HandleSetRendererBinding(SubParams);
		else if (OpName == TEXT("request_compile")) SubResult = HandleRequestCompile(SubParams);
		else if (OpName == TEXT("get_system_diagnostics")) SubResult = HandleGetSystemDiagnostics(SubParams);
		else if (OpName == TEXT("get_system_property")) SubResult = HandleGetSystemProperty(SubParams);
		else if (OpName == TEXT("set_system_property")) SubResult = HandleSetSystemProperty(SubParams);
		else if (OpName == TEXT("set_static_switch_value")) SubResult = HandleSetStaticSwitchValue(SubParams);
		// Wave 2
		else if (OpName == TEXT("get_system_summary")) SubResult = HandleGetSystemSummary(SubParams);
		else if (OpName == TEXT("get_emitter_summary")) SubResult = HandleGetEmitterSummary(SubParams);
		else if (OpName == TEXT("list_emitter_properties")) SubResult = HandleListEmitterProperties(SubParams);
		else if (OpName == TEXT("get_module_input_value")) SubResult = HandleGetModuleInputValue(SubParams);
		// Wave 3
		else if (OpName == TEXT("configure_curve_keys")) SubResult = HandleConfigureCurveKeys(SubParams);
		else if (OpName == TEXT("configure_data_interface")) SubResult = HandleConfigureDataInterface(SubParams);
		// Wave 4
		else if (OpName == TEXT("duplicate_system")) SubResult = HandleDuplicateSystem(SubParams);
		else if (OpName == TEXT("set_fixed_bounds")) SubResult = HandleSetFixedBounds(SubParams);
		else if (OpName == TEXT("set_effect_type")) SubResult = HandleSetEffectType(SubParams);
		else if (OpName == TEXT("create_emitter")) SubResult = HandleCreateEmitter(SubParams);
		else if (OpName == TEXT("export_system_spec")) SubResult = HandleExportSystemSpec(SubParams);
		// Wave 5
		else if (OpName == TEXT("add_dynamic_input")) SubResult = HandleAddDynamicInput(SubParams);
		else if (OpName == TEXT("set_dynamic_input_value")) SubResult = HandleSetDynamicInputValue(SubParams);
		else if (OpName == TEXT("search_dynamic_inputs")) SubResult = HandleSearchDynamicInputs(SubParams);
		// Wave 6
		else if (OpName == TEXT("add_event_handler")) SubResult = HandleAddEventHandler(SubParams);
		else if (OpName == TEXT("validate_system")) SubResult = HandleValidateSystem(SubParams);
		else if (OpName == TEXT("add_simulation_stage")) SubResult = HandleAddSimulationStage(SubParams);

		RO->SetBoolField(TEXT("success"), SubResult.bSuccess);
		if (!SubResult.bSuccess) RO->SetStringField(TEXT("error"), SubResult.ErrorMessage);
		// Include result data for read/getter operations so batch callers can see return values
		if (SubResult.bSuccess && SubResult.Result.IsValid())
		{
			RO->SetObjectField(TEXT("data"), SubResult.Result);
		}
		Results.Add(MakeShared<FJsonValueObject>(RO));
		if (SubResult.bSuccess) Ok++; else Fail++;
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), Fail == 0);
	Final->SetNumberField(TEXT("total"), Ops.Num());
	Final->SetNumberField(TEXT("succeeded"), Ok);
	Final->SetNumberField(TEXT("failed"), Fail);
	Final->SetArrayField(TEXT("results"), Results);
	return SuccessObj(Final);
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateSystemFromSpec(const TSharedPtr<FJsonObject>& Params)
{
	// Bug 1 fix: "spec" arrives as a parsed JSON object — don't serialize to string then re-parse.
	TSharedPtr<FJsonValue> SpecField = Params->TryGetField(TEXT("spec"));
	if (!SpecField.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: spec"));

	TSharedPtr<FJsonObject> Spec;
	if (SpecField->Type == EJson::Object)
	{
		Spec = SpecField->AsObject();
	}
	else if (SpecField->Type == EJson::String)
	{
		// Fallback: caller pre-serialized the object to a string
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
			return FMonolithActionResult::Error(TEXT("Failed to parse spec string"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'spec' must be an object"));
	}

	if (!Spec.IsValid())
		return FMonolithActionResult::Error(TEXT("Failed to parse spec"));

	// Accept save_path at top-level params (intuitive) or inside spec (legacy)
	FString SavePath;
	if (Params->HasField(TEXT("save_path")))
		SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty() && Spec->HasField(TEXT("save_path")))
		SavePath = Spec->GetStringField(TEXT("save_path"));
	FString Template = Spec->HasField(TEXT("template")) ? Spec->GetStringField(TEXT("template")) : FString();
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("save_path required (provide at params root or inside spec)"));

	// Create system
	TSharedRef<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), SavePath);
	if (!Template.IsEmpty()) CreateParams->SetStringField(TEXT("template"), Template);
	FMonolithActionResult CreateResult = HandleCreateSystem(CreateParams);
	if (!CreateResult.bSuccess) return CreateResult;

	FString SystemPath = CreateResult.Result.IsValid() ? CreateResult.Result->GetStringField(TEXT("result")) : FString();
	int32 FailCount = 0;
	TArray<FString> FailedOps;

	// Add user parameters
	if (Spec->HasField(TEXT("user_parameters")))
	{
		for (const TSharedPtr<FJsonValue>& PV : Spec->GetArrayField(TEXT("user_parameters")))
		{
			TSharedPtr<FJsonObject> PO = PV->AsObject();
			if (!PO) continue;
			TSharedRef<FJsonObject> AP = MakeShared<FJsonObject>();
			AP->SetStringField(TEXT("system_path"), SystemPath);
			AP->SetStringField(TEXT("name"), PO->GetStringField(TEXT("name")));
			AP->SetStringField(TEXT("type"), PO->GetStringField(TEXT("type")));
			if (PO->HasField(TEXT("default"))) AP->SetField(TEXT("default"), PO->TryGetField(TEXT("default")));
			FMonolithActionResult AUP = HandleAddUserParameter(AP);
			if (!AUP.bSuccess) { FailedOps.Add(FString::Printf(TEXT("add_user_parameter: %s"), *AUP.ErrorMessage)); FailCount++; }
		}
	}

	// Add emitters
	if (Spec->HasField(TEXT("emitters")))
	{
		for (const TSharedPtr<FJsonValue>& EV : Spec->GetArrayField(TEXT("emitters")))
		{
			TSharedPtr<FJsonObject> EO = EV->AsObject();
			if (!EO) continue;

			TSharedRef<FJsonObject> AEP = MakeShared<FJsonObject>();
			AEP->SetStringField(TEXT("system_path"), SystemPath);
			AEP->SetStringField(TEXT("emitter_asset"), EO->GetStringField(TEXT("asset")));
			if (EO->HasField(TEXT("name"))) AEP->SetStringField(TEXT("name"), EO->GetStringField(TEXT("name")));
			FMonolithActionResult AER = HandleAddEmitter(AEP);
			if (!AER.bSuccess) { FailedOps.Add(FString::Printf(TEXT("add_emitter: %s"), *AER.ErrorMessage)); FailCount++; continue; }

			// Parse handle_id from result
			FString EmitterId;
			if (AER.Result.IsValid())
				EmitterId = AER.Result->GetStringField(TEXT("handle_id"));
			if (EmitterId.IsEmpty()) continue;

			// Force synchronous compile so the stack graph is fully wired before adding modules.
			// AddEmitterToSystem calls RebuildEmitterNodes, but the graph may not be fully initialized
			// until a compile pass runs. Modules need the ParameterMap chain to be complete.
			UNiagaraSystem* SpecSystem = LoadSystem(SystemPath);
			if (SpecSystem)
			{
				SpecSystem->RequestCompile(true);
				SpecSystem->WaitForCompilationComplete();
			}

			// Emitter properties
			if (EO->HasField(TEXT("properties")))
			{
				TSharedPtr<FJsonObject> Props = EO->GetObjectField(TEXT("properties"));
				for (auto& P : Props->Values)
				{
					TSharedRef<FJsonObject> SP = MakeShared<FJsonObject>();
					SP->SetStringField(TEXT("system_path"), SystemPath);
					SP->SetStringField(TEXT("emitter"), EmitterId);
					SP->SetStringField(TEXT("property"), P.Key);
					SP->SetField(TEXT("value"), P.Value);
					FMonolithActionResult EPR = HandleSetEmitterProperty(SP);
					if (!EPR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("set_emitter_property[%s]: %s"), *P.Key, *EPR.ErrorMessage)); FailCount++; }
				}
			}

			// Modules
			if (EO->HasField(TEXT("modules")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Mods = EO->GetArrayField(TEXT("modules"));
				for (int32 MI = 0; MI < Mods.Num(); ++MI)
				{
					TSharedPtr<FJsonObject> MO = Mods[MI]->AsObject();
					if (!MO) continue;
					TSharedRef<FJsonObject> AMP = MakeShared<FJsonObject>();
					AMP->SetStringField(TEXT("system_path"), SystemPath);
					AMP->SetStringField(TEXT("emitter"), EmitterId);
					AMP->SetStringField(TEXT("usage"), MO->GetStringField(TEXT("stage")));
					AMP->SetStringField(TEXT("module_script"), MO->GetStringField(TEXT("script")));
					AMP->SetNumberField(TEXT("index"), MO->HasField(TEXT("index")) ? MO->GetNumberField(TEXT("index")) : MI);
					FMonolithActionResult AMR = HandleAddModule(AMP);
					if (!AMR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("add_module[%s]: %s"), *MO->GetStringField(TEXT("script")), *AMR.ErrorMessage)); FailCount++; continue; }

					FString NodeGuid;
					if (AMR.Result.IsValid())
						NodeGuid = AMR.Result->GetStringField(TEXT("node_guid"));
					if (NodeGuid.IsEmpty()) continue;

					if (MO->HasField(TEXT("inputs")))
					{
						TSharedPtr<FJsonObject> Ins = MO->GetObjectField(TEXT("inputs"));
						for (auto& IP : Ins->Values)
						{
							TSharedRef<FJsonObject> SIP = MakeShared<FJsonObject>();
							SIP->SetStringField(TEXT("system_path"), SystemPath);
							SIP->SetStringField(TEXT("emitter"), EmitterId);
							SIP->SetStringField(TEXT("module_node"), NodeGuid);
							SIP->SetStringField(TEXT("input"), IP.Key);
							SIP->SetField(TEXT("value"), IP.Value);
							FMonolithActionResult SIVR = HandleSetModuleInputValue(SIP);
						if (!SIVR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("set_module_input[%s]: %s"), *IP.Key, *SIVR.ErrorMessage)); FailCount++; }
						}
					}
					if (MO->HasField(TEXT("bindings")))
					{
						TSharedPtr<FJsonObject> Binds = MO->GetObjectField(TEXT("bindings"));
						for (auto& BP2 : Binds->Values)
						{
							TSharedRef<FJsonObject> SBP = MakeShared<FJsonObject>();
							SBP->SetStringField(TEXT("system_path"), SystemPath);
							SBP->SetStringField(TEXT("emitter"), EmitterId);
							SBP->SetStringField(TEXT("module_node"), NodeGuid);
							SBP->SetStringField(TEXT("input"), BP2.Key);
							SBP->SetStringField(TEXT("binding"), BP2.Value->AsString());
							FMonolithActionResult SIBR = HandleSetModuleInputBinding(SBP);
						if (!SIBR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("set_module_binding[%s]: %s"), *BP2.Key, *SIBR.ErrorMessage)); FailCount++; }
						}
					}
				}
			}

			// Renderers
			if (EO->HasField(TEXT("renderers")))
			{
				for (const TSharedPtr<FJsonValue>& RV : EO->GetArrayField(TEXT("renderers")))
				{
					TSharedPtr<FJsonObject> RO = RV->AsObject();
					if (!RO) continue;
					TSharedRef<FJsonObject> ARP = MakeShared<FJsonObject>();
					ARP->SetStringField(TEXT("system_path"), SystemPath);
					ARP->SetStringField(TEXT("emitter"), EmitterId);
					ARP->SetStringField(TEXT("class"), RO->GetStringField(TEXT("class")));
					FMonolithActionResult ARR = HandleAddRenderer(ARP);
					if (!ARR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("add_renderer[%s]: %s"), *RO->GetStringField(TEXT("class")), *ARR.ErrorMessage)); FailCount++; continue; }

					int32 RIdx = -1;
					if (ARR.Result.IsValid())
						RIdx = static_cast<int32>(ARR.Result->GetNumberField(TEXT("renderer_index")));
					if (RIdx < 0) continue;

					if (RO->HasField(TEXT("material")))
					{
						TSharedRef<FJsonObject> SMP = MakeShared<FJsonObject>();
						SMP->SetStringField(TEXT("system_path"), SystemPath);
						SMP->SetStringField(TEXT("emitter"), EmitterId);
						SMP->SetNumberField(TEXT("renderer_index"), RIdx);
						SMP->SetStringField(TEXT("material"), RO->GetStringField(TEXT("material")));
						FMonolithActionResult SRMR = HandleSetRendererMaterial(SMP);
					if (!SRMR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("set_renderer_material: %s"), *SRMR.ErrorMessage)); FailCount++; }
					}
					if (RO->HasField(TEXT("properties")))
					{
						TSharedPtr<FJsonObject> RProps = RO->GetObjectField(TEXT("properties"));
						for (auto& RP : RProps->Values)
						{
							TSharedRef<FJsonObject> SRP = MakeShared<FJsonObject>();
							SRP->SetStringField(TEXT("system_path"), SystemPath);
							SRP->SetStringField(TEXT("emitter"), EmitterId);
							SRP->SetNumberField(TEXT("renderer_index"), RIdx);
							SRP->SetStringField(TEXT("property"), RP.Key);
							SRP->SetField(TEXT("value"), RP.Value);
							FMonolithActionResult SRPR = HandleSetRendererProperty(SRP);
						if (!SRPR.bSuccess) { FailedOps.Add(FString::Printf(TEXT("set_renderer_property[%s]: %s"), *RP.Key, *SRPR.ErrorMessage)); FailCount++; }
						}
					}
				}
			}
		}
	}

	// Final compile
	TSharedRef<FJsonObject> CP = MakeShared<FJsonObject>();
	CP->SetStringField(TEXT("system_path"), SystemPath);
	HandleRequestCompile(CP);

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), FailCount == 0);
	Final->SetStringField(TEXT("system_path"), SystemPath);
	Final->SetNumberField(TEXT("failed_steps"), FailCount);
	if (FailedOps.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& E : FailedOps)
			ErrArr.Add(MakeShared<FJsonValueString>(E));
		Final->SetArrayField(TEXT("errors"), ErrArr);
	}
	return SuccessObj(Final);
}

// ============================================================================
// DI Actions (1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetDIFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString DIClassName = Params->GetStringField(TEXT("di_class"));

	FString ClassName = DIClassName;
	if (!ClassName.StartsWith(TEXT("U"))) ClassName = TEXT("U") + ClassName;
	if (!ClassName.Contains(TEXT("DataInterface"))) ClassName = TEXT("UNiagara") + DIClassName + TEXT("DataInterface");

	UClass* DIC = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (!DIC) DIC = FindFirstObject<UClass>(*ClassName.Mid(1), EFindFirstObjectOptions::NativeFirst);
	if (!DIC) DIC = FindFirstObject<UClass>(*DIClassName, EFindFirstObjectOptions::NativeFirst);
	if (!DIC)
	{
		FString NP = TEXT("UNiagara") + DIClassName;
		DIC = FindFirstObject<UClass>(*NP, EFindFirstObjectOptions::NativeFirst);
		if (!DIC) DIC = FindFirstObject<UClass>(*NP.Mid(1), EFindFirstObjectOptions::NativeFirst);
	}
	// Try UE convention: UNiagaraDataInterface<Name>
	if (!DIC) DIC = FindFirstObject<UClass>(*FString::Printf(TEXT("UNiagaraDataInterface%s"), *DIClassName), EFindFirstObjectOptions::NativeFirst);
	if (!DIC) DIC = FindFirstObject<UClass>(*FString::Printf(TEXT("NiagaraDataInterface%s"), *DIClassName), EFindFirstObjectOptions::NativeFirst);
	if (!DIC || !DIC->IsChildOf(UNiagaraDataInterface::StaticClass()))
		return FMonolithActionResult::Error(TEXT("DI class not found"));

	UNiagaraDataInterface* TempDI = NewObject<UNiagaraDataInterface>(GetTransientPackage(), DIC);
	if (!TempDI) return FMonolithActionResult::Error(TEXT("Failed to create DI instance"));

	TArray<FNiagaraFunctionSignature> Sigs;
	TempDI->GetFunctionSignatures(Sigs);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FNiagaraFunctionSignature& Sig : Sigs)
	{
		TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
		SO->SetStringField(TEXT("name"), Sig.Name.ToString());

		TArray<TSharedPtr<FJsonValue>> InsArr;
		for (const FNiagaraVariable& In : Sig.Inputs)
		{
			TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
			IO->SetStringField(TEXT("name"), In.GetName().ToString());
			IO->SetStringField(TEXT("type"), In.GetType().GetName());
			InsArr.Add(MakeShared<FJsonValueObject>(IO));
		}
		SO->SetArrayField(TEXT("inputs"), InsArr);

		TArray<TSharedPtr<FJsonValue>> OutsArr;
		for (const FNiagaraVariableBase& Out : Sig.Outputs)
		{
			TSharedRef<FJsonObject> OO = MakeShared<FJsonObject>();
			OO->SetStringField(TEXT("name"), Out.GetName().ToString());
			OO->SetStringField(TEXT("type"), Out.GetType().GetName());
			OutsArr.Add(MakeShared<FJsonValueObject>(OO));
		}
		SO->SetArrayField(TEXT("outputs"), OutsArr);

		SO->SetBoolField(TEXT("requires_exec_pin"), Sig.bRequiresExecPin);
		SO->SetBoolField(TEXT("member_function"), Sig.bMemberFunction);
		SO->SetBoolField(TEXT("requires_context"), Sig.bRequiresContext);
		SO->SetBoolField(TEXT("supports_gpu"), Sig.bSupportsGPU);
		SO->SetBoolField(TEXT("supports_cpu"), Sig.bSupportsCPU);

		FText Desc = Sig.GetDescription();
		if (!Desc.IsEmpty()) SO->SetStringField(TEXT("description"), Desc.ToString());

		Arr.Add(MakeShared<FJsonValueObject>(SO));
	}
	return SuccessStr(JsonArrayToString(Arr));
}

// ============================================================================
// HLSL Actions (1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetCompiledGPUHLSL(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	if (ED->SimTarget != ENiagaraSimTarget::GPUComputeSim)
		return FMonolithActionResult::Error(TEXT("Emitter is not GPU simulation"));

	UNiagaraScript* GPU = ED->GetGPUComputeScript();
	if (!GPU) return FMonolithActionResult::Error(TEXT("No GPU compute script"));

	// Auto-compile if HLSL data is not available
#if WITH_EDITORONLY_DATA
	{
		const FNiagaraVMExecutableData& PreCheck = GPU->GetVMExecutableData();
		if (PreCheck.LastHlslTranslationGPU.IsEmpty() && PreCheck.LastHlslTranslation.IsEmpty())
		{
			UE_LOG(LogMonolithNiagara, Log, TEXT("HLSL not available — requesting compilation"));
			System->RequestCompile(false);
			System->WaitForCompilationComplete(true, false); // true = include GPU shaders, false = no progress dialog
		}
	}
#endif

	if (System->HasOutstandingCompilationRequests())
		UE_LOG(LogMonolithNiagara, Warning, TEXT("System has outstanding compilation requests"));

	FString HLSL;
#if WITH_EDITORONLY_DATA
	const FNiagaraVMExecutableData& ExeData = GPU->GetVMExecutableData();
	if (!ExeData.LastHlslTranslationGPU.IsEmpty()) HLSL = ExeData.LastHlslTranslationGPU;
	else if (!ExeData.LastHlslTranslation.IsEmpty()) HLSL = ExeData.LastHlslTranslation;
	else if (!ExeData.LastAssemblyTranslation.IsEmpty()) HLSL = ExeData.LastAssemblyTranslation;
	else return FMonolithActionResult::Error(TEXT("Compilation completed but produced no GPU HLSL. The emitter may have compile errors or may not use GPU simulation."));
#else
	return FMonolithActionResult::Error(TEXT("HLSL only available in editor builds"));
#endif

	return SuccessStr(HLSL);
}

// ============================================================================
// Read Actions (2)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleListEmitters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterArr;
	EmitterArr.Reserve(Handles.Num());

	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();

		TSharedRef<FJsonObject> EObj = MakeShared<FJsonObject>();
		EObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EObj->SetStringField(TEXT("id"), Handle.GetId().ToString());
		EObj->SetNumberField(TEXT("index"), i);
		EObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

		if (ED)
		{
			// Sim target
			FString SimTarget;
			switch (ED->SimTarget)
			{
			case ENiagaraSimTarget::CPUSim:          SimTarget = TEXT("CPUSim"); break;
			case ENiagaraSimTarget::GPUComputeSim:   SimTarget = TEXT("GPUComputeSim"); break;
			default:                                 SimTarget = TEXT("Unknown"); break;
			}
			EObj->SetStringField(TEXT("sim_target"), SimTarget);

			// Renderer count
			EObj->SetNumberField(TEXT("renderer_count"), ED->GetRenderers().Num());
		}
		else
		{
			EObj->SetStringField(TEXT("sim_target"), TEXT("Unknown"));
			EObj->SetNumberField(TEXT("renderer_count"), 0);
		}

		EmitterArr.Add(MakeShared<FJsonValueObject>(EObj));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetArrayField(TEXT("emitters"), EmitterArr);
	R->SetNumberField(TEXT("count"), Handles.Num());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleListRenderers(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
	TArray<TSharedPtr<FJsonValue>> RendArr;
	RendArr.Reserve(Renderers.Num());

	for (int32 i = 0; i < Renderers.Num(); ++i)
	{
		UNiagaraRendererProperties* Rend = Renderers[i];
		if (!Rend) continue;

		TSharedRef<FJsonObject> RObj = MakeShared<FJsonObject>();
		RObj->SetNumberField(TEXT("index"), i);
		FString ClassName = Rend->GetClass()->GetName();
		RObj->SetStringField(TEXT("class"), ClassName);
		// Add short name that add_renderer accepts directly
		FString ShortName = TEXT("unknown");
		if (ClassName.Contains(TEXT("Sprite"))) ShortName = TEXT("sprite");
		else if (ClassName.Contains(TEXT("Mesh"))) ShortName = TEXT("mesh");
		else if (ClassName.Contains(TEXT("Ribbon"))) ShortName = TEXT("ribbon");
		else if (ClassName.Contains(TEXT("Light"))) ShortName = TEXT("light");
		else if (ClassName.Contains(TEXT("Component"))) ShortName = TEXT("component");
		RObj->SetStringField(TEXT("type"), ShortName);
		RObj->SetBoolField(TEXT("enabled"), Rend->GetIsEnabled());

		// Extract material path if applicable
		UMaterialInterface* Mat = nullptr;
		if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend))
		{
			Mat = S->Material;
		}
		else if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend))
		{
			Mat = Rib->Material;
		}
		else if (UNiagaraMeshRendererProperties* M = Cast<UNiagaraMeshRendererProperties>(Rend))
		{
			if (M->bOverrideMaterials && M->OverrideMaterials.Num() > 0)
			{
				Mat = M->OverrideMaterials[0].ExplicitMat;
			}
		}
		else
		{
			// Generic fallback: check for a "Material" property
			FProperty* MP = Rend->GetClass()->FindPropertyByName(TEXT("Material"));
			if (FObjectProperty* OP = CastField<FObjectProperty>(MP))
			{
				Mat = Cast<UMaterialInterface>(OP->GetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(Rend)));
			}
		}

		if (Mat)
		{
			RObj->SetStringField(TEXT("material"), Mat->GetPathName());
		}

		RendArr.Add(MakeShared<FJsonValueObject>(RObj));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetArrayField(TEXT("renderers"), RendArr);
	R->SetNumberField(TEXT("count"), Renderers.Num());
	return SuccessObj(R);
}

// ============================================================================
// Diagnostics (1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetSystemDiagnostics(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Optionally force a synchronous compile first (default: true)
	bool bCompileFirst = true;
	if (Params->HasField(TEXT("compile_first")))
	{
		bCompileFirst = Params->GetBoolField(TEXT("compile_first"));
	}
	if (bCompileFirst)
	{
		System->RequestCompile(true);
		System->WaitForCompilationComplete();
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Info;

	auto MakeDiag = [](const FString& Emitter, const FString& Source, const FString& Message) -> TSharedPtr<FJsonValue>
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("emitter"), Emitter);
		Obj->SetStringField(TEXT("source"), Source);
		Obj->SetStringField(TEXT("message"), Message);
		return MakeShared<FJsonValueObject>(Obj);
	};

	// ---- Category A: Compile events from script executable data ----
	auto GatherScriptEvents = [&](UNiagaraScript* Script, const FString& EmitterLabel, const FString& ScriptLabel)
	{
		if (!Script) return;
		const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();
		if (!VMData.IsValid())
		{
			Errors.Add(MakeDiag(EmitterLabel, ScriptLabel, TEXT("Data missing — script has no compiled bytecode (force recompile or recreate)")));
			return;
		}
		for (const FNiagaraCompileEvent& Event : VMData.LastCompileEvents)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("emitter"), EmitterLabel);
			Obj->SetStringField(TEXT("source"), ScriptLabel);
			Obj->SetStringField(TEXT("message"), Event.Message);
			if (!Event.ShortDescription.IsEmpty())
				Obj->SetStringField(TEXT("short"), Event.ShortDescription);

			TSharedPtr<FJsonValue> Val = MakeShared<FJsonValueObject>(Obj);
			switch (Event.Severity)
			{
			case FNiagaraCompileEventSeverity::Error:   Errors.Add(Val);   break;
			case FNiagaraCompileEventSeverity::Warning: Warnings.Add(Val); break;
			default:                                    Info.Add(Val);     break;
			}
		}
	};

	// System-level scripts
	GatherScriptEvents(System->GetSystemSpawnScript(), TEXT("System"), TEXT("SystemSpawnScript"));
	GatherScriptEvents(System->GetSystemUpdateScript(), TEXT("System"), TEXT("SystemUpdateScript"));

	// Per-emitter scripts
	TArray<TSharedPtr<FJsonValue>> EmitterStats;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;
		FString EmitterName = Handle.GetName().ToString();

		TArray<UNiagaraScript*> Scripts;
		ED->GetScripts(Scripts, true);  // bCompilableOnly — skip editor-only emitter spawn/update scripts (inlined into system scripts)
		for (UNiagaraScript* S : Scripts)
		{
			if (!S) continue;
			FString ScriptLabel = FString::Printf(TEXT("%s_%s"),
				*StaticEnum<ENiagaraScriptUsage>()->GetDisplayNameTextByValue(static_cast<int64>(S->GetUsage())).ToString(),
				S->GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript ? TEXT("GPU") : TEXT("VM"));
			GatherScriptEvents(S, EmitterName, ScriptLabel);
		}

		// ---- Category B: Renderer compatibility ----
		for (UNiagaraRendererProperties* Renderer : ED->GetRenderers())
		{
			if (!Renderer || !Renderer->GetIsEnabled()) continue;

			if (!Renderer->IsSimTargetSupported(ED->SimTarget))
			{
				Errors.Add(MakeDiag(EmitterName,
					FString::Printf(TEXT("Renderer:%s"), *Renderer->GetClass()->GetName()),
					FString::Printf(TEXT("Renderer incompatible with chosen Sim Target (%s)"),
						ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"))));
			}

#if WITH_EDITOR
			// Detailed renderer feedback
			FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
			TArray<FNiagaraRendererFeedback> RErrors, RWarnings, RInfos;
			Renderer->GetRendererFeedback(VersionedEmitter, RErrors, RWarnings, RInfos);
			for (const FNiagaraRendererFeedback& F : RErrors)
				Errors.Add(MakeDiag(EmitterName, FString::Printf(TEXT("Renderer:%s"), *Renderer->GetClass()->GetName()), F.GetDescriptionText().ToString()));
			for (const FNiagaraRendererFeedback& F : RWarnings)
				Warnings.Add(MakeDiag(EmitterName, FString::Printf(TEXT("Renderer:%s"), *Renderer->GetClass()->GetName()), F.GetDescriptionText().ToString()));
#endif
		}

		// ---- Category C: Bounds mode validation ----
		if (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim
			&& ED->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic
			&& !System->bFixedBounds)
		{
			Warnings.Add(MakeDiag(EmitterName, TEXT("EmitterProperties"),
				TEXT("GPU emitter using dynamic bounds mode — bounds may be incorrect. Set CalculateBoundsMode to Fixed or enable system fixed bounds.")));
		}

		// ---- Category D: Script stats (instruction count, resources) ----
		TSharedRef<FJsonObject> EStats = MakeShared<FJsonObject>();
		EStats->SetStringField(TEXT("emitter"), EmitterName);
		EStats->SetStringField(TEXT("sim_target"), ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));

		TArray<TSharedPtr<FJsonValue>> ScriptStatsArr;
		for (UNiagaraScript* S : Scripts)
		{
			if (!S) continue;
			TSharedRef<FJsonObject> SS = MakeShared<FJsonObject>();
			FString UsageName = StaticEnum<ENiagaraScriptUsage>()->GetDisplayNameTextByValue(static_cast<int64>(S->GetUsage())).ToString();
			SS->SetStringField(TEXT("script"), UsageName);

			const FNiagaraVMExecutableData& VMData = S->GetVMExecutableData();
			if (VMData.IsValid())
			{
				SS->SetBoolField(TEXT("valid"), true);
				SS->SetNumberField(TEXT("op_count"), VMData.LastOpCount);
				SS->SetNumberField(TEXT("register_count"), VMData.NumTempRegisters);
				// Compile status
				FString StatusStr;
				switch (VMData.LastCompileStatus)
				{
				case ENiagaraScriptCompileStatus::NCS_Error:            StatusStr = TEXT("Error"); break;
				case ENiagaraScriptCompileStatus::NCS_UpToDate:         StatusStr = TEXT("UpToDate"); break;
				case ENiagaraScriptCompileStatus::NCS_Dirty:            StatusStr = TEXT("Dirty"); break;
				case ENiagaraScriptCompileStatus::NCS_BeingCreated:     StatusStr = TEXT("BeingCreated"); break;
				case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings: StatusStr = TEXT("UpToDateWithWarnings"); break;
				case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: StatusStr = TEXT("ComputeUpToDateWithWarnings"); break;
				default:                                                StatusStr = TEXT("Unknown"); break;
				}
				SS->SetStringField(TEXT("compile_status"), StatusStr);
			}
			else
			{
				SS->SetBoolField(TEXT("valid"), false);
				SS->SetStringField(TEXT("compile_status"), TEXT("DataMissing"));
			}

			ScriptStatsArr.Add(MakeShared<FJsonValueObject>(SS));
		}
		EStats->SetArrayField(TEXT("scripts"), ScriptStatsArr);
		EmitterStats.Add(MakeShared<FJsonValueObject>(EStats));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("error_count"), Errors.Num());
	R->SetNumberField(TEXT("warning_count"), Warnings.Num());
	R->SetArrayField(TEXT("errors"), Errors);
	R->SetArrayField(TEXT("warnings"), Warnings);
	R->SetArrayField(TEXT("info"), Info);
	R->SetArrayField(TEXT("emitter_stats"), EmitterStats);
	R->SetBoolField(TEXT("has_issues"), Errors.Num() > 0 || Warnings.Num() > 0);
	return SuccessObj(R);
}

// ============================================================================
// list_module_scripts — Search available Niagara module scripts via Asset Registry
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleListModuleScripts(const TSharedPtr<FJsonObject>& Params)
{
	FString Search = Params->HasField(TEXT("search")) ? Params->GetStringField(TEXT("search")) : TEXT("");
	FString UsageFilter = Params->HasField(TEXT("usage")) ? Params->GetStringField(TEXT("usage")).ToLower() : TEXT("");
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString AssetName = Asset.AssetName.ToString();
		FString PackagePath = Asset.GetSoftObjectPath().ToString();

		// Infer usage from path patterns (asset registry tags for NiagaraScript usage are unreliable)
		FString InferredUsage = TEXT("unknown");
		if (PackagePath.Contains(TEXT("/Modules/"))) InferredUsage = TEXT("module");
		else if (PackagePath.Contains(TEXT("/Functions/"))) InferredUsage = TEXT("function");
		else if (PackagePath.Contains(TEXT("/DynamicInputs/"))) InferredUsage = TEXT("dynamic_input");

		// Filter by usage if specified
		if (!UsageFilter.IsEmpty() && UsageFilter != TEXT("all") && InferredUsage != UsageFilter) continue;

		// Filter by search keyword — tokenize on spaces so "gravity force" matches "GravityForce"
		if (!Search.IsEmpty())
		{
			TArray<FString> Tokens;
			Search.ParseIntoArray(Tokens, TEXT(" "), true);
			bool bAllMatch = true;
			for (const FString& Token : Tokens)
			{
				if (!AssetName.Contains(Token, ESearchCase::IgnoreCase)
					&& !PackagePath.Contains(Token, ESearchCase::IgnoreCase))
				{
					bAllMatch = false;
					break;
				}
			}
			if (!bAllMatch) continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("path"), PackagePath);
		Entry->SetStringField(TEXT("usage"), InferredUsage);
		Results.Add(MakeShared<FJsonValueObject>(Entry));

		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetNumberField(TEXT("total_scanned"), Assets.Num());
	R->SetArrayField(TEXT("scripts"), Results);
	if (Search.IsEmpty() && Results.Num() >= Limit)
	{
		R->SetStringField(TEXT("note"), TEXT("Results truncated. Use 'search' to narrow down, or increase 'limit'."));
	}
	return SuccessObj(R);
}

// ============================================================================
// list_renderer_properties — List editable properties on a renderer
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleListRendererProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
	if (!Renderers.IsValidIndex(RendererIndex))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Renderer index %d out of range (0-%d)"), RendererIndex, Renderers.Num() - 1));

	UNiagaraRendererProperties* Rend = Renderers[RendererIndex];
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer is null"));

	TArray<TSharedPtr<FJsonValue>> PropArr;
	for (TFieldIterator<FProperty> It(Rend->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue; // Only editable properties

		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), Prop->GetName());
		PO->SetStringField(TEXT("type"), Prop->GetCPPType());
		PO->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));

		// Get current value as string
		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(Rend), nullptr, Rend, PPF_None);
		if (ValueStr.Len() <= 200) // Don't include massive values
		{
			PO->SetStringField(TEXT("value"), ValueStr);
		}

		PropArr.Add(MakeShared<FJsonValueObject>(PO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("renderer_class"), Rend->GetClass()->GetName());
	R->SetNumberField(TEXT("property_count"), PropArr.Num());
	R->SetArrayField(TEXT("properties"), PropArr);
	return SuccessObj(R);
}

// ============================================================================
// Action: set_system_property
// ============================================================================

// Shared alias map for system property name resolution
static const TMap<FString, FString> SystemPropertyAliases = {
	{ TEXT("warmup_time"), TEXT("WarmupTime") },
	{ TEXT("warmup_tick_count"), TEXT("WarmupTickCount") },
	{ TEXT("warmup_tick_delta"), TEXT("WarmupTickDelta") },
	{ TEXT("fixed_tick_delta"), TEXT("bFixedTickDelta") },
	{ TEXT("fixed_tick_delta_time"), TEXT("FixedTickDeltaTime") },
	{ TEXT("determinism"), TEXT("bDeterminism") },
	{ TEXT("random_seed"), TEXT("RandomSeed") },
	{ TEXT("max_pool_size"), TEXT("MaxPoolSize") },
	{ TEXT("support_large_world_coordinates"), TEXT("bSupportLargeWorldCoordinates") },
};

FMonolithActionResult FMonolithNiagaraActions::HandleGetSystemProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString PropertyName = Params->GetStringField(TEXT("property"));
	if (PropertyName.IsEmpty()) PropertyName = Params->GetStringField(TEXT("property_name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	FString ResolvedName = PropertyName;
	if (const FString* Alias = SystemPropertyAliases.Find(PropertyName))
		ResolvedName = *Alias;

	// Dedicated getters for known properties
	if (ResolvedName == TEXT("WarmupTime"))
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("property"), ResolvedName);
		R->SetNumberField(TEXT("value"), System->GetWarmupTime());
		return SuccessObj(R);
	}
	if (ResolvedName == TEXT("WarmupTickDelta"))
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("property"), ResolvedName);
		R->SetNumberField(TEXT("value"), System->GetWarmupTickDelta());
		return SuccessObj(R);
	}

	// Generic reflection readback
	FProperty* Prop = System->GetClass()->FindPropertyByName(*ResolvedName);
	if (!Prop)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown property '%s'. Use snake_case aliases (warmup_time, determinism, random_seed, max_pool_size) or UProperty names."), *PropertyName));

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(System);
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("property"), ResolvedName);

	if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
		R->SetNumberField(TEXT("value"), FP->GetPropertyValue(ValuePtr));
	else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
		R->SetNumberField(TEXT("value"), DP->GetPropertyValue(ValuePtr));
	else if (FIntProperty* IP = CastField<FIntProperty>(Prop))
		R->SetNumberField(TEXT("value"), IP->GetPropertyValue(ValuePtr));
	else if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		R->SetBoolField(TEXT("value"), BP->GetPropertyValue(ValuePtr));
	else
	{
		FString ExportedValue;
		Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, System, PPF_None);
		R->SetStringField(TEXT("value"), ExportedValue);
	}

	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetSystemProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString PropertyName = Params->GetStringField(TEXT("property"));
	if (PropertyName.IsEmpty()) PropertyName = Params->GetStringField(TEXT("property_name"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetSysProp", "Set System Property"));
	System->Modify();
	bool bOk = false;

	FString ResolvedName = PropertyName;
	if (const FString* Alias = SystemPropertyAliases.Find(PropertyName))
		ResolvedName = *Alias;

	// Use dedicated setters where available (they handle recompile triggers)
	if (ResolvedName == TEXT("WarmupTime"))
	{ System->SetWarmupTime(static_cast<float>(JV->AsNumber())); bOk = true; }
	else if (ResolvedName == TEXT("WarmupTickDelta"))
	{ System->SetWarmupTickDelta(static_cast<float>(JV->AsNumber())); bOk = true; }
	else
	{
		// Generic property reflection for everything else
		FProperty* Prop = System->GetClass()->FindPropertyByName(*ResolvedName);
		if (Prop)
		{
			FString ValStr;
			if (JV->Type == EJson::Number) ValStr = FString::SanitizeFloat(JV->AsNumber());
			else if (JV->Type == EJson::Boolean) ValStr = JV->AsBool() ? TEXT("true") : TEXT("false");
			else ValStr = JV->AsString();

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(System);
			if (Prop->ImportText_Direct(*ValStr, ValuePtr, System, PPF_None))
			{
				bOk = true;
			}
		}
	}

	GEditor->EndTransaction();
	if (bOk)
	{
		FPropertyChangedEvent PCE(nullptr);
		System->PostEditChangeProperty(PCE);
		System->RequestCompile(false);
	}
	return bOk ? SuccessStr(TEXT("System property set")) : FMonolithActionResult::Error(
		FString::Printf(TEXT("Unknown property '%s'. Supported: WarmupTime, WarmupTickCount, WarmupTickDelta, bFixedTickDelta, FixedTickDeltaTime, bDeterminism, RandomSeed, MaxPoolSize, or any UNiagaraSystem UProperty name."), *PropertyName));
}

// ============================================================================
// Action: set_static_switch_value
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSetStaticSwitchValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	FString InputName = Params->GetStringField(TEXT("input"));
	if (InputName.IsEmpty()) InputName = Params->GetStringField(TEXT("input_name"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid())
		return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found"), *EmitterHandleId));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Enumerate static switch inputs from the module's script graph (NOT GetStackFunctionInputs which only returns data inputs)
	UNiagaraGraph* CalledGraph = MN->GetCalledGraph();
	if (!CalledGraph)
		return FMonolithActionResult::Error(TEXT("Module has no script graph — cannot enumerate static switches"));

	TArray<FNiagaraVariable> SwitchInputs = CalledGraph->FindStaticSwitchInputs();

	FNiagaraTypeDefinition InputType;
	FName MatchedFullName;
	bool bInputFound = false;
	FString InputNameNoSpaces = InputName;
	InputNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	for (const FNiagaraVariable& In : SwitchInputs)
	{
		FString VarName = In.GetName().ToString();
		bool bMatch = VarName.Equals(InputName, ESearchCase::IgnoreCase);
		if (!bMatch)
		{
			FString VarNameNoSpaces = VarName;
			VarNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = VarNameNoSpaces.Equals(InputNameNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}

	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : SwitchInputs) { ValidNames.Add(In.GetName().ToString()); }
		if (ValidNames.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Input '%s' not found — this module has no static switches"), *InputName));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Static switch '%s' not found. Valid static switches: [%s]"), *InputName, *FString::Join(ValidNames, TEXT(", "))));
	}

	// Static switch pins live directly on the FunctionCall node — find by matching variable name
	UEdGraphPin* SwitchPin = nullptr;
	for (UEdGraphPin* Pin : MN->Pins)
	{
		if (Pin->Direction == EGPD_Input && Pin->GetFName() == MatchedFullName)
		{
			SwitchPin = Pin;
			break;
		}
	}
	if (!SwitchPin)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Static switch pin '%s' not found on module node. The switch exists in the script but has no corresponding pin."), *InputName));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetStaticSwitch", "Set Static Switch"));
	System->Modify();

	// Break existing links
	if (SwitchPin->LinkedTo.Num() > 0)
	{
		SwitchPin->BreakAllPinLinks();
	}

	// Set the value
	FString ValStr;
	if (JV->Type == EJson::Number) ValStr = FString::SanitizeFloat(JV->AsNumber());
	else if (JV->Type == EJson::Boolean) ValStr = JV->AsBool() ? TEXT("true") : TEXT("false");
	else ValStr = JV->AsString();

	SwitchPin->DefaultValue = ValStr;

	GEditor->EndTransaction();
	System->RequestCompile(false);

	return SuccessStr(FString::Printf(TEXT("Static switch '%s' set to '%s'"), *InputName, *ValStr));
}

// ============================================================================
// Wave 2: Summary & Discovery Actions (4 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetSystemSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("system_name"), System->GetName());

	// System properties
	TSharedRef<FJsonObject> SysProps = MakeShared<FJsonObject>();
	SysProps->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	FProperty* DetProp = System->GetClass()->FindPropertyByName(TEXT("bDeterminism"));
	if (DetProp)
	{
		const void* DetAddr = DetProp->ContainerPtrToValuePtr<void>(System);
		if (FBoolProperty* BP = CastField<FBoolProperty>(DetProp))
			SysProps->SetBoolField(TEXT("determinism"), BP->GetPropertyValue(DetAddr));
	}
	SysProps->SetBoolField(TEXT("fixed_bounds"), System->bFixedBounds != 0);
	UNiagaraEffectType* ET = System->GetEffectType();
	SysProps->SetStringField(TEXT("effect_type"), ET ? ET->GetPathName() : TEXT("null"));
	R->SetObjectField(TEXT("system_properties"), SysProps);

	// User parameters
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);
	TArray<TSharedPtr<FJsonValue>> UserParamsArr;
	for (const FNiagaraVariable& P : UP)
	{
		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), P.GetName().ToString());
		PO->SetStringField(TEXT("type"), P.GetType().GetName());
		PO->SetStringField(TEXT("default"), SerializeParameterValue(P, US));
		UserParamsArr.Add(MakeShared<FJsonValueObject>(PO));
	}
	R->SetArrayField(TEXT("user_parameters"), UserParamsArr);

	// Emitters
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TArray<TSharedPtr<FJsonValue>> EmitterArr;
	int32 TotalModuleCount = 0;
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		TSharedRef<FJsonObject> EObj = MakeShared<FJsonObject>();
		EObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EObj->SetNumberField(TEXT("index"), i);
		EObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EObj->SetStringField(TEXT("sim_target"), ED && ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));
		if (ED) EObj->SetBoolField(TEXT("local_space"), ED->bLocalSpace != 0);

		// Module counts per usage
		TSharedRef<FJsonObject> MCounts = MakeShared<FJsonObject>();
		static const TPair<ENiagaraScriptUsage, const TCHAR*> Usages[] = {
			{ENiagaraScriptUsage::EmitterSpawnScript, TEXT("emitter_spawn")},
			{ENiagaraScriptUsage::EmitterUpdateScript, TEXT("emitter_update")},
			{ENiagaraScriptUsage::ParticleSpawnScript, TEXT("particle_spawn")},
			{ENiagaraScriptUsage::ParticleUpdateScript, TEXT("particle_update")},
		};
		int32 EmitterModuleCount = 0;
		for (const auto& [Usage, Name] : Usages)
		{
			UNiagaraNodeOutput* Out = FindOutputNode(System, Handle.GetId().ToString(), Usage);
			int32 Count = 0;
			if (Out)
			{
				TArray<UNiagaraNodeFunctionCall*> Mods;
				MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
				Count = Mods.Num();
			}
			MCounts->SetNumberField(Name, Count);
			EmitterModuleCount += Count;
		}
		EObj->SetObjectField(TEXT("module_count"), MCounts);
		TotalModuleCount += EmitterModuleCount;

		// Renderer info
		if (ED)
		{
			const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
			EObj->SetNumberField(TEXT("renderer_count"), Renderers.Num());
			TArray<TSharedPtr<FJsonValue>> RendTypes;
			for (UNiagaraRendererProperties* Rend : Renderers)
			{
				if (Rend) RendTypes.Add(MakeShared<FJsonValueString>(Rend->GetClass()->GetName()));
			}
			EObj->SetArrayField(TEXT("renderer_types"), RendTypes);
		}
		EmitterArr.Add(MakeShared<FJsonValueObject>(EObj));
	}
	R->SetArrayField(TEXT("emitters"), EmitterArr);
	R->SetNumberField(TEXT("emitter_count"), Handles.Num());
	R->SetNumberField(TEXT("total_module_count"), TotalModuleCount);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetEmitterSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("name"), Handle.GetName().ToString());
	R->SetStringField(TEXT("sim_target"), ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));
	R->SetBoolField(TEXT("local_space"), ED->bLocalSpace != 0);
	R->SetBoolField(TEXT("determinism"), ED->bDeterminism != 0);

	// Modules per stage
	TSharedRef<FJsonObject> ModulesObj = MakeShared<FJsonObject>();
	static const TPair<ENiagaraScriptUsage, const TCHAR*> Usages[] = {
		{ENiagaraScriptUsage::EmitterSpawnScript, TEXT("emitter_spawn")},
		{ENiagaraScriptUsage::EmitterUpdateScript, TEXT("emitter_update")},
		{ENiagaraScriptUsage::ParticleSpawnScript, TEXT("particle_spawn")},
		{ENiagaraScriptUsage::ParticleUpdateScript, TEXT("particle_update")},
	};
	FString HandleId = Handle.GetId().ToString();
	for (const auto& [Usage, Name] : Usages)
	{
		UNiagaraNodeOutput* Out = FindOutputNode(System, HandleId, Usage);
		TArray<TSharedPtr<FJsonValue>> ModArr;
		if (Out)
		{
			TArray<UNiagaraNodeFunctionCall*> Mods;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
			for (UNiagaraNodeFunctionCall* MN : Mods)
			{
				if (!MN) continue;
				TSharedRef<FJsonObject> MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("name"), MN->GetFunctionName());
				MO->SetStringField(TEXT("guid"), MN->NodeGuid.ToString());
				MO->SetBoolField(TEXT("enabled"), MN->IsNodeEnabled());
				ModArr.Add(MakeShared<FJsonValueObject>(MO));
			}
		}
		ModulesObj->SetArrayField(Name, ModArr);
	}
	R->SetObjectField(TEXT("modules"), ModulesObj);

	// Renderers
	TArray<TSharedPtr<FJsonValue>> RendArr;
	const TArray<UNiagaraRendererProperties*>& Renderers = ED->GetRenderers();
	for (int32 i = 0; i < Renderers.Num(); ++i)
	{
		UNiagaraRendererProperties* Rend = Renderers[i];
		if (!Rend) continue;
		TSharedRef<FJsonObject> RO = MakeShared<FJsonObject>();
		RO->SetNumberField(TEXT("index"), i);
		RO->SetStringField(TEXT("type"), Rend->GetClass()->GetName());
		// Extract material
		UMaterialInterface* Mat = nullptr;
		if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend)) Mat = S->Material;
		else if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend)) Mat = Rib->Material;
		if (Mat) RO->SetStringField(TEXT("material"), Mat->GetPathName());
		RendArr.Add(MakeShared<FJsonValueObject>(RO));
	}
	R->SetArrayField(TEXT("renderers"), RendArr);

	// Event handlers
	TArray<TSharedPtr<FJsonValue>> EventArr;
	for (const FNiagaraEventScriptProperties& ESP : ED->EventHandlerScriptProps)
	{
		TSharedRef<FJsonObject> EO = MakeShared<FJsonObject>();
		EO->SetStringField(TEXT("event_name"), ESP.SourceEventName.ToString());
		EO->SetStringField(TEXT("source_emitter_id"), ESP.SourceEmitterID.ToString());
		EventArr.Add(MakeShared<FJsonValueObject>(EO));
	}
	R->SetArrayField(TEXT("event_handlers"), EventArr);

	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleListEmitterProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const UScriptStruct* EmitterDataStruct = FVersionedNiagaraEmitterData::StaticStruct();
	TArray<TSharedPtr<FJsonValue>> PropArr;
	for (TFieldIterator<FProperty> PropIt(EmitterDataStruct); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), Prop->GetName());
		PO->SetStringField(TEXT("type"), Prop->GetCPPType());

		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(ED), nullptr, nullptr, PPF_None);
		if (ValueStr.Len() <= 200) PO->SetStringField(TEXT("value"), ValueStr);

		PropArr.Add(MakeShared<FJsonValueObject>(PO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("property_count"), PropArr.Num());
	R->SetArrayField(TEXT("properties"), PropArr);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	FString InputName = Params->GetStringField(TEXT("input"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Get all inputs via engine API
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// Find the matching input
	FName InputFName(*InputName);
	FString InputNameNoSpaces = InputName;
	InputNameNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	FNiagaraTypeDefinition InputType;
	FName MatchedFullName;
	bool bInputFound = false;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		bool bMatch = (ShortName == InputFName || In.GetName() == InputFName);
		if (!bMatch)
		{
			FString ShortStr = ShortName.ToString();
			ShortStr.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = ShortStr.Equals(InputNameNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}
	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : Inputs) ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString());
		return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found. Valid: [%s]"), *InputName, *FString::Join(ValidNames, TEXT(", "))));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("input"), MonolithNiagaraHelpers::StripModulePrefix(MatchedFullName).ToString());
	R->SetStringField(TEXT("type"), InputType.GetName());

	// Check for static switch pin first
	for (UEdGraphPin* Pin : MN->Pins)
	{
		if (Pin->Direction == EGPD_Input && Pin->GetFName() == MatchedFullName)
		{
			R->SetStringField(TEXT("value"), Pin->DefaultValue);
			R->SetBoolField(TEXT("is_default"), false);
			R->SetBoolField(TEXT("is_linked"), false);
			R->SetBoolField(TEXT("is_dynamic_input"), false);
			R->SetStringField(TEXT("source"), TEXT("static_switch"));
			return SuccessObj(R);
		}
	}

	// Walk override pin for data inputs
	FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedFullName), MN);
	UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*MN, AH);

	if (!OP)
	{
		R->SetStringField(TEXT("value"), TEXT("(default)"));
		R->SetBoolField(TEXT("is_default"), true);
		R->SetBoolField(TEXT("is_linked"), false);
		R->SetBoolField(TEXT("is_dynamic_input"), false);
		return SuccessObj(R);
	}

	if (OP->LinkedTo.Num() > 0)
	{
		UEdGraphNode* UpstreamNode = OP->LinkedTo[0]->GetOwningNode();
		if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(UpstreamNode))
		{
			// DI input
			FObjectProperty* DIProp = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface"));
			UNiagaraDataInterface* DI = DIProp
				? Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue(DIProp->ContainerPtrToValuePtr<void>(InputNode)))
				: nullptr;
			R->SetBoolField(TEXT("is_default"), false);
			R->SetBoolField(TEXT("is_linked"), true);
			R->SetBoolField(TEXT("is_dynamic_input"), false);
			R->SetStringField(TEXT("source"), TEXT("data_interface"));
			if (DI) R->SetStringField(TEXT("di_class"), DI->GetClass()->GetName());
		}
		else if (UNiagaraNodeFunctionCall* FCNode = Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
		{
			R->SetBoolField(TEXT("is_default"), false);
			R->SetBoolField(TEXT("is_linked"), false);
			R->SetBoolField(TEXT("is_dynamic_input"), true);
			R->SetStringField(TEXT("dynamic_input_name"), FCNode->GetFunctionName());
			R->SetStringField(TEXT("dynamic_input_guid"), FCNode->NodeGuid.ToString());
		}
		else
		{
			// Bound parameter (ParameterMapGet)
			R->SetBoolField(TEXT("is_default"), false);
			R->SetBoolField(TEXT("is_linked"), true);
			R->SetBoolField(TEXT("is_dynamic_input"), false);
			R->SetStringField(TEXT("linked_parameter"), OP->LinkedTo[0]->PinName.ToString());
		}
	}
	else
	{
		// Literal override
		R->SetStringField(TEXT("value"), OP->DefaultValue);
		R->SetBoolField(TEXT("is_default"), false);
		R->SetBoolField(TEXT("is_linked"), false);
		R->SetBoolField(TEXT("is_dynamic_input"), false);
	}

	return SuccessObj(R);
}

// ============================================================================
// Wave 3: DI Curve & Configuration Actions (2 new)
// ============================================================================

// Helper: find DI from override pin upstream chain
UNiagaraDataInterface* FMonolithNiagaraActions::FindDIFromOverridePin(UNiagaraNodeFunctionCall* ModuleNode, const FName& MatchedFullName, const FNiagaraTypeDefinition& InputType)
{
	FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedFullName), ModuleNode);
	UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*ModuleNode, AH);
	if (!OP || OP->LinkedTo.Num() == 0) return nullptr;

	UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(OP->LinkedTo[0]->GetOwningNode());
	if (!InputNode) return nullptr;

	FObjectProperty* DIProp = FindFProperty<FObjectProperty>(InputNode->GetClass(), TEXT("DataInterface"));
	if (!DIProp) return nullptr;
	return Cast<UNiagaraDataInterface>(DIProp->GetObjectPropertyValue(DIProp->ContainerPtrToValuePtr<void>(InputNode)));
}

// Helper: find function call node by GUID across all emitter graphs (for dynamic input nodes)
UNiagaraNodeFunctionCall* FMonolithNiagaraActions::FindFunctionCallNode(UNiagaraSystem* System, const FString& EmitterHandleId, const FString& NodeGuidStr)
{
	FGuid TargetGuid;
	if (!FGuid::Parse(NodeGuidStr, TargetGuid)) return nullptr;

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return nullptr;

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return nullptr;

	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);
	if (!Source || !Source->NodeGraph) return nullptr;

	TArray<UNiagaraNodeFunctionCall*> AllFCs;
	Source->NodeGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllFCs);
	for (UNiagaraNodeFunctionCall* FC : AllFCs)
	{
		if (FC && FC->NodeGuid == TargetGuid) return FC;
	}

	// Also check system graph
	UNiagaraScript* SysScript = System->GetSystemSpawnScript();
	if (SysScript)
	{
		UNiagaraScriptSource* SysSrc = Cast<UNiagaraScriptSource>(SysScript->GetLatestSource());
		if (SysSrc && SysSrc->NodeGraph)
		{
			TArray<UNiagaraNodeFunctionCall*> SysFCs;
			SysSrc->NodeGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(SysFCs);
			for (UNiagaraNodeFunctionCall* FC : SysFCs)
			{
				if (FC && FC->NodeGuid == TargetGuid) return FC;
			}
		}
	}

	return nullptr;
}

FMonolithActionResult FMonolithNiagaraActions::HandleConfigureCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	FString InputName = Params->GetStringField(TEXT("input"));
	FString InterpStr = Params->HasField(TEXT("interp")) ? Params->GetStringField(TEXT("interp")).ToLower() : TEXT("cubic");

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Resolve input
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	FName InputFName(*InputName);
	FString InputNoSpaces = InputName;
	InputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	FNiagaraTypeDefinition InputType;
	FName MatchedFullName;
	bool bFound = false;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName Short = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		bool bMatch = (Short == InputFName || In.GetName() == InputFName);
		if (!bMatch)
		{
			FString S = Short.ToString();
			S.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = S.Equals(InputNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch) { InputType = In.GetType(); MatchedFullName = In.GetName(); bFound = true; break; }
	}
	if (!bFound) return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found"), *InputName));
	if (!InputType.IsDataInterface()) return FMonolithActionResult::Error(TEXT("Input is not a DataInterface type. Use set_module_input_di to create one first."));

	// Find existing DI
	UNiagaraDataInterface* DI = FindDIFromOverridePin(MN, MatchedFullName, InputType);
	bool bCreatedOverride = false;
	if (!DI)
	{
		// Path B: no override exists — create one via set_module_input_di pattern
		UClass* ExpectedDIClass = const_cast<UClass*>(InputType.GetClass());
		if (!ExpectedDIClass) return FMonolithActionResult::Error(TEXT("Cannot determine DI class for this input"));

		// Create DI and assign via SetDataInterfaceValueForFunctionInput
		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(MatchedFullName), MN);
		GEditor->BeginTransaction(NSLOCTEXT("Monolith", "CreateCurveDI", "Create Curve DI"));
		System->Modify();
		UEdGraphPin& OP = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			*MN, AH, InputType, FGuid(), FGuid());
		if (OP.LinkedTo.Num() > 0) OP.BreakAllPinLinks();
		FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OP, ExpectedDIClass, InputName, DI);
		GEditor->EndTransaction();
		bCreatedOverride = true;
	}

	if (!DI) return FMonolithActionResult::Error(TEXT("Failed to find or create curve DI on this input"));

	// Parse keys
	TSharedPtr<FJsonValue> KeysField = Params->TryGetField(TEXT("keys"));
	if (!KeysField.IsValid()) return FMonolithActionResult::Error(TEXT("Missing required field: keys"));
	TArray<TSharedPtr<FJsonValue>> Keys;
	if (KeysField->Type == EJson::Array) Keys = KeysField->AsArray();
	else if (KeysField->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(KeysField->AsString());
		FJsonSerializer::Deserialize(Reader, Keys);
	}
	if (Keys.Num() == 0) return FMonolithActionResult::Error(TEXT("keys array is empty"));

	// Determine interp mode
	ERichCurveInterpMode InterpMode = RCIM_Cubic;
	if (InterpStr == TEXT("linear")) InterpMode = RCIM_Linear;
	else if (InterpStr == TEXT("constant")) InterpMode = RCIM_Constant;

	// Build config JSON and apply via existing helper
	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();

	// Check if keys contain color channels (r,g,b,a) or vector channels (x,y,z)
	bool bHasColor = false, bHasVector = false;
	if (Keys.Num() > 0)
	{
		TSharedPtr<FJsonObject> FirstKey = Keys[0]->AsObject();
		if (FirstKey.IsValid())
		{
			bHasColor = FirstKey->HasField(TEXT("r")) || FirstKey->HasField(TEXT("g"));
			bHasVector = !bHasColor && (FirstKey->HasField(TEXT("x")) || FirstKey->HasField(TEXT("y")));
		}
	}

	if (bHasColor)
	{
		// Split into per-channel arrays for color curve
		TArray<TSharedPtr<FJsonValue>> RedKeys, GreenKeys, BlueKeys, AlphaKeys;
		for (const TSharedPtr<FJsonValue>& KV : Keys)
		{
			TSharedPtr<FJsonObject> KO = KV->AsObject();
			if (!KO) continue;
			float Time = static_cast<float>(KO->GetNumberField(TEXT("time")));
			auto MakeKey = [Time, InterpMode](float Val) -> TSharedPtr<FJsonValue>
			{
				TSharedRef<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"), Time);
				K->SetNumberField(TEXT("value"), Val);
				K->SetStringField(TEXT("interp_mode"), InterpMode == RCIM_Linear ? TEXT("linear") : InterpMode == RCIM_Constant ? TEXT("constant") : TEXT("cubic"));
				return MakeShared<FJsonValueObject>(K);
			};
			if (KO->HasField(TEXT("r"))) RedKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("r")))));
			if (KO->HasField(TEXT("g"))) GreenKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("g")))));
			if (KO->HasField(TEXT("b"))) BlueKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("b")))));
			if (KO->HasField(TEXT("a"))) AlphaKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("a")))));
		}
		if (RedKeys.Num() > 0) Config->SetField(TEXT("red"), MakeShared<FJsonValueArray>(RedKeys));
		if (GreenKeys.Num() > 0) Config->SetField(TEXT("green"), MakeShared<FJsonValueArray>(GreenKeys));
		if (BlueKeys.Num() > 0) Config->SetField(TEXT("blue"), MakeShared<FJsonValueArray>(BlueKeys));
		if (AlphaKeys.Num() > 0) Config->SetField(TEXT("alpha"), MakeShared<FJsonValueArray>(AlphaKeys));
	}
	else if (bHasVector)
	{
		TArray<TSharedPtr<FJsonValue>> XKeys, YKeys, ZKeys, WKeys;
		for (const TSharedPtr<FJsonValue>& KV : Keys)
		{
			TSharedPtr<FJsonObject> KO = KV->AsObject();
			if (!KO) continue;
			float Time = static_cast<float>(KO->GetNumberField(TEXT("time")));
			auto MakeKey = [Time, InterpMode](float Val) -> TSharedPtr<FJsonValue>
			{
				TSharedRef<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetNumberField(TEXT("time"), Time);
				K->SetNumberField(TEXT("value"), Val);
				K->SetStringField(TEXT("interp_mode"), InterpMode == RCIM_Linear ? TEXT("linear") : InterpMode == RCIM_Constant ? TEXT("constant") : TEXT("cubic"));
				return MakeShared<FJsonValueObject>(K);
			};
			if (KO->HasField(TEXT("x"))) XKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("x")))));
			if (KO->HasField(TEXT("y"))) YKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("y")))));
			if (KO->HasField(TEXT("z"))) ZKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("z")))));
			if (KO->HasField(TEXT("w"))) WKeys.Add(MakeKey(static_cast<float>(KO->GetNumberField(TEXT("w")))));
		}
		if (XKeys.Num() > 0) Config->SetField(TEXT("x"), MakeShared<FJsonValueArray>(XKeys));
		if (YKeys.Num() > 0) Config->SetField(TEXT("y"), MakeShared<FJsonValueArray>(YKeys));
		if (ZKeys.Num() > 0) Config->SetField(TEXT("z"), MakeShared<FJsonValueArray>(ZKeys));
		if (WKeys.Num() > 0) Config->SetField(TEXT("w"), MakeShared<FJsonValueArray>(WKeys));
	}
	else
	{
		// Float curve — set interp on each key
		TArray<TSharedPtr<FJsonValue>> FloatKeys;
		for (const TSharedPtr<FJsonValue>& KV : Keys)
		{
			TSharedPtr<FJsonObject> KO = KV->AsObject();
			if (!KO) continue;
			TSharedRef<FJsonObject> NK = MakeShared<FJsonObject>();
			NK->SetNumberField(TEXT("time"), KO->GetNumberField(TEXT("time")));
			NK->SetNumberField(TEXT("value"), KO->GetNumberField(TEXT("value")));
			NK->SetStringField(TEXT("interp_mode"), InterpMode == RCIM_Linear ? TEXT("linear") : InterpMode == RCIM_Constant ? TEXT("constant") : TEXT("cubic"));
			FloatKeys.Add(MakeShared<FJsonValueObject>(NK));
		}
		Config->SetField(TEXT("keys"), MakeShared<FJsonValueArray>(FloatKeys));
	}

	bool bApplied = MonolithNiagaraHelpers::ApplyCurveConfig(DI, Config);
	if (!bApplied) return FMonolithActionResult::Error(TEXT("Failed to apply curve keys — DI type may not be a supported curve type"));

	// Refresh GPU LUT
	if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DI))
	{
#if WITH_EDITORONLY_DATA
		CurveBase->UpdateLUT();
#endif
	}

	DI->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("di_class"), DI->GetClass()->GetName());
	Result->SetNumberField(TEXT("key_count"), Keys.Num());
	Result->SetBoolField(TEXT("created_override"), bCreatedOverride);
	return SuccessObj(Result);
}

FMonolithActionResult FMonolithNiagaraActions::HandleConfigureDataInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	FString InputName = Params->GetStringField(TEXT("input"));

	TSharedPtr<FJsonValue> PropsField = Params->TryGetField(TEXT("properties"));
	TSharedPtr<FJsonObject> Properties = PropsField.IsValid() ? AsObjectOrParseString(PropsField) : nullptr;
	if (!Properties.IsValid() || Properties->Values.Num() == 0)
		return FMonolithActionResult::Error(TEXT("Missing or empty 'properties' object"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Resolve input
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	FName InputFName(*InputName);
	FString InputNoSpaces = InputName;
	InputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	FNiagaraTypeDefinition InputType;
	FName MatchedFullName;
	bool bInputFound = false;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName Short = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		bool bMatch = (Short == InputFName || In.GetName() == InputFName);
		if (!bMatch)
		{
			FString S = Short.ToString();
			S.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = S.Equals(InputNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}
	if (!bInputFound) return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found"), *InputName));

	UNiagaraDataInterface* DI = FindDIFromOverridePin(MN, MatchedFullName, InputType);
	if (!DI) return FMonolithActionResult::Error(TEXT("No DataInterface found on this input. Use set_module_input_di to create one first."));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "ConfigDI", "Configure Data Interface"));
	System->Modify();

	TArray<FString> PropsSet;
	TArray<FString> PropsNotFound;
	for (auto& Pair : Properties->Values)
	{
		FProperty* Prop = DI->GetClass()->FindPropertyByName(FName(*Pair.Key));
		if (!Prop) { PropsNotFound.Add(Pair.Key); continue; }
		void* Addr = Prop->ContainerPtrToValuePtr<void>(DI);

		FString ValStr;
		if (Pair.Value->Type == EJson::Number) ValStr = FString::SanitizeFloat(Pair.Value->AsNumber());
		else if (Pair.Value->Type == EJson::Boolean) ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
		else ValStr = Pair.Value->AsString();

		if (Prop->ImportText_Direct(*ValStr, Addr, DI, PPF_None))
		{
			PropsSet.Add(Pair.Key);
		}
	}

	// Refresh GPU LUT for curve DIs
	if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DI))
	{
#if WITH_EDITORONLY_DATA
		CurveBase->UpdateLUT();
#endif
	}

	DI->MarkPackageDirty();
	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("di_class"), DI->GetClass()->GetName());
	TArray<TSharedPtr<FJsonValue>> SetArr;
	for (const FString& S : PropsSet) SetArr.Add(MakeShared<FJsonValueString>(S));
	R->SetArrayField(TEXT("properties_set"), SetArr);
	if (PropsNotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& S : PropsNotFound) NotFoundArr.Add(MakeShared<FJsonValueString>(S));
		R->SetArrayField(TEXT("properties_not_found"), NotFoundArr);
	}
	return SuccessObj(R);
}

// ============================================================================
// Wave 4: System Management Actions (5 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleDuplicateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: save_path"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load source system"));

	// Parse save_path into package path + asset name
	int32 LastSlash;
	if (!SavePath.FindLastChar('/', LastSlash))
		return FMonolithActionResult::Error(TEXT("Invalid save_path — must contain '/'"));
	FString DestPath = SavePath.Left(LastSlash);
	FString NewName = SavePath.Mid(LastSlash + 1);

	FAssetToolsModule& ATModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* Dup = ATModule.Get().DuplicateAsset(NewName, DestPath, System);
	UNiagaraSystem* DupSystem = Cast<UNiagaraSystem>(Dup);
	if (!DupSystem) return FMonolithActionResult::Error(TEXT("DuplicateAsset failed"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("asset_path"), DupSystem->GetPathName());
	R->SetNumberField(TEXT("emitter_count"), DupSystem->GetEmitterHandles().Num());
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetFixedBounds(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->HasField(TEXT("emitter")) ? Params->GetStringField(TEXT("emitter")) : FString();
	bool bEnabled = !Params->HasField(TEXT("enabled")) || Params->GetBoolField(TEXT("enabled"));

	// Parse min/max arrays
	auto ParseVec3 = [](const TSharedPtr<FJsonObject>& P, const FString& Key) -> TOptional<FVector>
	{
		TSharedPtr<FJsonValue> Field = P->TryGetField(Key);
		if (!Field.IsValid()) return {};
		TArray<TSharedPtr<FJsonValue>> Arr;
		if (Field->Type == EJson::Array) Arr = Field->AsArray();
		else if (Field->Type == EJson::String)
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Field->AsString());
			FJsonSerializer::Deserialize(Reader, Arr);
		}
		if (Arr.Num() < 3) return {};
		return FVector(Arr[0]->AsNumber(), Arr[1]->AsNumber(), Arr[2]->AsNumber());
	};

	TOptional<FVector> MinV = ParseVec3(Params, TEXT("min"));
	TOptional<FVector> MaxV = ParseVec3(Params, TEXT("max"));
	if (!MinV.IsSet() || !MaxV.IsSet())
		return FMonolithActionResult::Error(TEXT("Both 'min' and 'max' are required as [x,y,z] arrays"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetBounds", "Set Fixed Bounds"));
	System->Modify();

	FString Level;
	if (EmitterHandleId.IsEmpty())
	{
		// System-level bounds
		System->bFixedBounds = bEnabled ? 1 : 0;
		if (bEnabled) System->SetFixedBounds(FBox(MinV.GetValue(), MaxV.GetValue()));
		Level = TEXT("system");
	}
	else
	{
		int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
		if (EIdx == INDEX_NONE) { GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("Emitter not found")); }
		FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
		if (!ED) { GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("No emitter data")); }
		ED->CalculateBoundsMode = bEnabled ? ENiagaraEmitterCalculateBoundMode::Fixed : ENiagaraEmitterCalculateBoundMode::Dynamic;
		if (bEnabled) ED->FixedBounds = FBox(MinV.GetValue(), MaxV.GetValue());
		Level = TEXT("emitter");
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("level"), Level);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEffectType(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EffectTypePath = Params->GetStringField(TEXT("effect_type"));
	if (EffectTypePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: effect_type"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetET", "Set Effect Type"));
	System->Modify();

	if (EffectTypePath.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		System->SetEffectType(nullptr);
	}
	else
	{
		UNiagaraEffectType* ET = FMonolithAssetUtils::LoadAssetByPath<UNiagaraEffectType>(EffectTypePath);
		if (!ET) { GEditor->EndTransaction(); return FMonolithActionResult::Error(TEXT("Failed to load effect type")); }
		System->SetEffectType(ET);
	}

	GEditor->EndTransaction();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("effect_type"), EffectTypePath);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterName = Params->GetStringField(TEXT("name"));
	FString SimTarget = Params->HasField(TEXT("sim_target")) ? Params->GetStringField(TEXT("sim_target")).ToLower() : TEXT("cpu");
	if (EmitterName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Use the Minimal emitter template
	static const FString MinimalTemplate = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Minimal");
	UNiagaraEmitter* EmitterAsset = FMonolithAssetUtils::LoadAssetByPath<UNiagaraEmitter>(MinimalTemplate);
	if (!EmitterAsset) return FMonolithActionResult::Error(TEXT("Failed to load Minimal emitter template"));

	TSharedRef<FJsonObject> AEP = MakeShared<FJsonObject>();
	AEP->SetStringField(TEXT("asset_path"), SystemPath);
	AEP->SetStringField(TEXT("emitter_asset"), MinimalTemplate);
	AEP->SetStringField(TEXT("name"), EmitterName);
	FMonolithActionResult AER = HandleAddEmitter(AEP);
	if (!AER.bSuccess) return AER;

	FString EmitterId;
	if (AER.Result.IsValid()) EmitterId = AER.Result->GetStringField(TEXT("handle_id"));

	// Set sim target if GPU
	if (SimTarget == TEXT("gpu") && !EmitterId.IsEmpty())
	{
		TSharedRef<FJsonObject> SP = MakeShared<FJsonObject>();
		SP->SetStringField(TEXT("asset_path"), SystemPath);
		SP->SetStringField(TEXT("emitter"), EmitterId);
		SP->SetStringField(TEXT("property"), TEXT("SimTarget"));
		SP->SetStringField(TEXT("value"), TEXT("GPUComputeSim"));
		HandleSetEmitterProperty(SP);
	}

	int32 EIdx = FindEmitterHandleIndex(System, EmitterId);
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("emitter_name"), EmitterName);
	R->SetNumberField(TEXT("emitter_index"), EIdx);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleExportSystemSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	bool bIncludeValues = !Params->HasField(TEXT("include_values")) || Params->GetBoolField(TEXT("include_values"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();

	// User parameters
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);
	TArray<TSharedPtr<FJsonValue>> UserParams;
	for (const FNiagaraVariable& P : UP)
	{
		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), P.GetName().ToString());
		PO->SetStringField(TEXT("type"), P.GetType().GetName());
		PO->SetStringField(TEXT("default"), SerializeParameterValue(P, US));
		UserParams.Add(MakeShared<FJsonValueObject>(PO));
	}
	Spec->SetArrayField(TEXT("user_parameters"), UserParams);

	// System properties
	TSharedRef<FJsonObject> SysProps = MakeShared<FJsonObject>();
	SysProps->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	Spec->SetObjectField(TEXT("system_properties"), SysProps);

	// Emitters
	TArray<TSharedPtr<FJsonValue>> EmittersArr;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;

		TSharedRef<FJsonObject> EO = MakeShared<FJsonObject>();
		EO->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EO->SetStringField(TEXT("sim_target"), ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));

		// Modules per stage
		TArray<TSharedPtr<FJsonValue>> ModulesArr;
		FString HandleId = Handle.GetId().ToString();
		static const TPair<ENiagaraScriptUsage, const TCHAR*> StageUsages[] = {
			{ENiagaraScriptUsage::EmitterSpawnScript, TEXT("emitter_spawn")},
			{ENiagaraScriptUsage::EmitterUpdateScript, TEXT("emitter_update")},
			{ENiagaraScriptUsage::ParticleSpawnScript, TEXT("particle_spawn")},
			{ENiagaraScriptUsage::ParticleUpdateScript, TEXT("particle_update")},
		};
		for (const auto& [Usage, StageName] : StageUsages)
		{
			UNiagaraNodeOutput* Out = FindOutputNode(System, HandleId, Usage);
			if (!Out) continue;
			TArray<UNiagaraNodeFunctionCall*> Mods;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
			for (UNiagaraNodeFunctionCall* MN : Mods)
			{
				if (!MN) continue;
				TSharedRef<FJsonObject> MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("stage"), StageName);
				FString ScriptPath;
				if (MN->FunctionScript)
					ScriptPath = MN->FunctionScript->GetPathName();
				MO->SetStringField(TEXT("script"), ScriptPath);

				if (bIncludeValues)
				{
					TSharedRef<FJsonObject> InputsObj = MakeShared<FJsonObject>();
					// Get override pin values
					TArray<FNiagaraVariable> ModInputs;
					int32 ExportEmitterIdx = FindEmitterHandleIndex(System, HandleId);
					if (ExportEmitterIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[ExportEmitterIdx].GetInstance();
						FCompileConstantResolver Resolver(VE, Usage);
						FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, ModInputs, Resolver,
							FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
					}
					for (const FNiagaraVariable& In : ModInputs)
					{
						FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
							FNiagaraParameterHandle(In.GetName()), MN);
						UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*MN, AH);
						if (OP && OP->LinkedTo.Num() == 0 && !OP->DefaultValue.IsEmpty())
						{
							FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
							InputsObj->SetStringField(ShortName.ToString(), OP->DefaultValue);
						}
					}
					if (InputsObj->Values.Num() > 0)
						MO->SetObjectField(TEXT("inputs"), InputsObj);
				}

				ModulesArr.Add(MakeShared<FJsonValueObject>(MO));
			}
		}
		EO->SetArrayField(TEXT("modules"), ModulesArr);

		// Renderers
		TArray<TSharedPtr<FJsonValue>> RendArr;
		for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
		{
			if (!Rend) continue;
			TSharedRef<FJsonObject> RO = MakeShared<FJsonObject>();
			RO->SetStringField(TEXT("class"), Rend->GetClass()->GetName());
			UMaterialInterface* Mat = nullptr;
			if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend)) Mat = S->Material;
			else if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend)) Mat = Rib->Material;
			if (Mat) RO->SetStringField(TEXT("material"), Mat->GetPathName());
			RendArr.Add(MakeShared<FJsonValueObject>(RO));
		}
		EO->SetArrayField(TEXT("renderers"), RendArr);

		EmittersArr.Add(MakeShared<FJsonValueObject>(EO));
	}
	Spec->SetArrayField(TEXT("emitters"), EmittersArr);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetObjectField(TEXT("spec"), Spec);
	return SuccessObj(R);
}

// ============================================================================
// Wave 5: Dynamic Input Actions (3 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddDynamicInput(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	FString InputName = Params->GetStringField(TEXT("input"));
	FString DynInputPath = Params->GetStringField(TEXT("dynamic_input_script"));

	if (DynInputPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: dynamic_input_script"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Resolve input
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	FName InputFName(*InputName);
	FString InputNoSpaces = InputName;
	InputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	FNiagaraTypeDefinition InputType;
	FName MatchedFullName;
	bool bInputFound = false;
	for (const FNiagaraVariable& In : Inputs)
	{
		FName Short = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
		bool bMatch = (Short == InputFName || In.GetName() == InputFName);
		if (!bMatch)
		{
			FString S = Short.ToString();
			S.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			bMatch = S.Equals(InputNoSpaces, ESearchCase::IgnoreCase);
		}
		if (bMatch) { InputType = In.GetType(); MatchedFullName = In.GetName(); bInputFound = true; break; }
	}
	if (!bInputFound) return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found"), *InputName));

	// Load the dynamic input script
	UNiagaraScript* DynScript = LoadObject<UNiagaraScript>(nullptr, *DynInputPath);
	if (!DynScript) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load dynamic input script: %s"), *DynInputPath));

	// Get or create override pin
	FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(MatchedFullName), MN);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddDynInput", "Add Dynamic Input"));
	System->Modify();

	UEdGraphPin& OP = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*MN, AH, InputType, FGuid(), FGuid());

	// Clean up existing links
	if (OP.LinkedTo.Num() > 0) OP.BreakAllPinLinks();

	// Set the dynamic input
	UNiagaraNodeFunctionCall* OutDynNode = nullptr;
	FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OP, DynScript, OutDynNode);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	if (!OutDynNode) return FMonolithActionResult::Error(TEXT("SetDynamicInputForFunctionInput returned null node"));

	// Read back the dynamic input's own inputs
	TArray<TSharedPtr<FJsonValue>> DynInputsArr;
	for (UEdGraphPin* Pin : OutDynNode->Pins)
	{
		if (Pin->Direction != EGPD_Input || Pin->bHidden) continue;
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
		TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
		IO->SetStringField(TEXT("name"), Pin->PinName.ToString());
		IO->SetStringField(TEXT("type"), PinType.GetName());
		IO->SetStringField(TEXT("default"), Pin->DefaultValue);
		DynInputsArr.Add(MakeShared<FJsonValueObject>(IO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("dynamic_input_node_guid"), OutDynNode->NodeGuid.ToString());
	R->SetStringField(TEXT("dynamic_input_name"), OutDynNode->GetFunctionName());
	R->SetArrayField(TEXT("inputs"), DynInputsArr);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetDynamicInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString DynNodeGuid = Params->GetStringField(TEXT("dynamic_input_node"));
	FString InputName = Params->GetStringField(TEXT("input"));
	FString Value = Params->GetStringField(TEXT("value"));

	if (DynNodeGuid.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: dynamic_input_node"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Find the dynamic input node by GUID across emitter graphs
	UNiagaraNodeFunctionCall* DynNode = FindFunctionCallNode(System, EmitterHandleId, DynNodeGuid);
	if (!DynNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Dynamic input node '%s' not found"), *DynNodeGuid));

	// Reuse set_module_input_value logic — construct params and delegate
	TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
	SubParams->SetStringField(TEXT("asset_path"), SystemPath);
	SubParams->SetStringField(TEXT("emitter"), EmitterHandleId);
	SubParams->SetStringField(TEXT("module_node"), DynNodeGuid);
	SubParams->SetStringField(TEXT("input"), InputName);
	SubParams->SetStringField(TEXT("value"), Value);
	return HandleSetModuleInputValue(SubParams);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSearchDynamicInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString Query = Params->HasField(TEXT("query")) ? Params->GetStringField(TEXT("query")) : TEXT("");
	FString InputType = Params->HasField(TEXT("input_type")) ? Params->GetStringField(TEXT("input_type")).ToLower() : TEXT("");
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 20;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString Path = Asset.GetSoftObjectPath().ToString();
		if (!Path.Contains(TEXT("/DynamicInputs/"))) continue;

		FString AssetName = Asset.AssetName.ToString();

		// Keyword filter
		if (!Query.IsEmpty())
		{
			TArray<FString> Tokens;
			Query.ParseIntoArray(Tokens, TEXT(" "), true);
			bool bAllMatch = true;
			for (const FString& Token : Tokens)
			{
				if (!AssetName.Contains(Token, ESearchCase::IgnoreCase) && !Path.Contains(Token, ESearchCase::IgnoreCase))
				{
					bAllMatch = false; break;
				}
			}
			if (!bAllMatch) continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("display_name"), AssetName);
		Entry->SetStringField(TEXT("script_path"), Path);

		// Infer output type from name patterns (lightweight — avoids loading the script)
		FString LowerName = AssetName.ToLower();
		FString InferredType = TEXT("unknown");
		if (LowerName.Contains(TEXT("float"))) InferredType = TEXT("float");
		else if (LowerName.Contains(TEXT("color"))) InferredType = TEXT("LinearColor");
		else if (LowerName.Contains(TEXT("vector"))) InferredType = TEXT("Vector");
		else if (LowerName.Contains(TEXT("int"))) InferredType = TEXT("int32");
		else if (LowerName.Contains(TEXT("bool"))) InferredType = TEXT("bool");
		Entry->SetStringField(TEXT("inferred_output_type"), InferredType);

		// Type filter
		if (!InputType.IsEmpty() && InferredType != TEXT("unknown") && !InferredType.Equals(InputType, ESearchCase::IgnoreCase))
			continue;

		Results.Add(MakeShared<FJsonValueObject>(Entry));
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetArrayField(TEXT("dynamic_inputs"), Results);
	return SuccessObj(R);
}

// ============================================================================
// Wave 6: Advanced Actions (3 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddEventHandler(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString EventName = Params->GetStringField(TEXT("event_name"));
	FString SourceEmitterStr = Params->HasField(TEXT("source_emitter")) ? Params->GetStringField(TEXT("source_emitter")) : FString();
	FString ExecModeStr = Params->HasField(TEXT("execution_mode")) ? Params->GetStringField(TEXT("execution_mode")).ToLower() : TEXT("every_particle");
	int32 MaxEvents = Params->HasField(TEXT("max_events_per_frame")) ? static_cast<int32>(Params->GetNumberField(TEXT("max_events_per_frame"))) : 0;
	int32 SpawnNum = Params->HasField(TEXT("spawn_number")) ? static_cast<int32>(Params->GetNumberField(TEXT("spawn_number"))) : 0;

	if (EventName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: event_name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	// Resolve execution mode
	EScriptExecutionMode ExecMode = EScriptExecutionMode::EveryParticle;
	if (ExecModeStr == TEXT("spawned_particles")) ExecMode = EScriptExecutionMode::SpawnedParticles;
	else if (ExecModeStr == TEXT("single_particle")) ExecMode = EScriptExecutionMode::SingleParticle;

	// Resolve source emitter GUID
	FGuid SourceEmitterGuid;
	if (!SourceEmitterStr.IsEmpty())
	{
		int32 SrcIdx = FindEmitterHandleIndex(System, SourceEmitterStr);
		if (SrcIdx == INDEX_NONE) return FMonolithActionResult::Error(FString::Printf(TEXT("Source emitter '%s' not found"), *SourceEmitterStr));
		SourceEmitterGuid = System->GetEmitterHandles()[SrcIdx].GetId();
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddEvtHandler", "Add Event Handler"));
	System->Modify();

	FNiagaraEventScriptProperties Handler;
	Handler.ExecutionMode = ExecMode;
	Handler.SpawnNumber = static_cast<uint32>(SpawnNum);
	Handler.MaxEventsPerFrame = static_cast<uint32>(MaxEvents);
	Handler.SourceEmitterID = SourceEmitterGuid;
	Handler.SourceEventName = FName(*EventName);

	ED->EventHandlerScriptProps.Add(Handler);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("message"), FString::Printf(TEXT("Added event handler for '%s'"), *EventName));
	R->SetNumberField(TEXT("handler_index"), ED->EventHandlerScriptProps.Num() - 1);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleValidateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TArray<TSharedPtr<FJsonValue>> Errors;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Suggestions;

	auto MakeEntry = [](const FString& Emitter, const FString& Msg) -> TSharedPtr<FJsonValue>
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("emitter"), Emitter);
		O->SetStringField(TEXT("message"), Msg);
		return MakeShared<FJsonValueObject>(O);
	};

	// System-level checks
	if (!System->IsValid())
		Errors.Add(MakeEntry(TEXT("System"), TEXT("System::IsValid() returned false")));

	// Per-emitter checks
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;
		FString EN = Handle.GetName().ToString();

		// GPU + Light Renderer = error
		if (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim)
		{
			for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
			{
				if (Rend && Cast<UNiagaraLightRendererProperties>(Rend))
				{
					Errors.Add(MakeEntry(EN, TEXT("Light Renderer on GPU emitter — Light Renderer requires CPU sim")));
				}
			}
		}

		// Missing material on sprite/ribbon renderer
		for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
		{
			if (!Rend || !Rend->GetIsEnabled()) continue;
			if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend))
			{
				if (!S->Material) Warnings.Add(MakeEntry(EN, TEXT("Sprite renderer has no material assigned")));
			}
			else if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend))
			{
				if (!Rib->Material) Warnings.Add(MakeEntry(EN, TEXT("Ribbon renderer has no material assigned")));
			}
			// Renderer-sim target compatibility
			if (!Rend->IsSimTargetSupported(ED->SimTarget))
			{
				Errors.Add(MakeEntry(EN, FString::Printf(TEXT("Renderer '%s' incompatible with sim target"), *Rend->GetClass()->GetName())));
			}
		}

		// No fixed bounds on GPU emitter
		if (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim
			&& ED->CalculateBoundsMode != ENiagaraEmitterCalculateBoundMode::Fixed
			&& !System->bFixedBounds)
		{
			Suggestions.Add(MakeEntry(EN, TEXT("GPU emitter without fixed bounds — bounds recalculated every frame")));
		}

		// Compile status per script
		TArray<UNiagaraScript*> Scripts;
		ED->GetScripts(Scripts, true);
		for (UNiagaraScript* S : Scripts)
		{
			if (!S) continue;
			const FNiagaraVMExecutableData& VMData = S->GetVMExecutableData();
			if (VMData.IsValid() && VMData.LastCompileStatus == ENiagaraScriptCompileStatus::NCS_Error)
			{
				for (const FNiagaraCompileEvent& Evt : VMData.LastCompileEvents)
				{
					if (Evt.Severity == FNiagaraCompileEventSeverity::Error)
						Errors.Add(MakeEntry(EN, Evt.Message));
				}
			}
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	R->SetArrayField(TEXT("errors"), Errors);
	R->SetArrayField(TEXT("warnings"), Warnings);
	R->SetArrayField(TEXT("suggestions"), Suggestions);
	return SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system '%s'"), *SystemPath));

	FString EmitterName = Params->GetStringField(TEXT("emitter"));
	int32 HandleIdx = FindEmitterHandleIndex(System, EmitterName);
	if (HandleIdx == INDEX_NONE) return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found"), *EmitterName));

	FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[HandleIdx];
	FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
	UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
	FVersionedNiagaraEmitterData* ED = Emitter->GetLatestEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("Failed to get emitter data"));

	// Get source — the emitter's GraphSource is the shared script source for all stages
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);

	GEditor->BeginTransaction(FText::FromString(TEXT("Add Simulation Stage")));
	Emitter->Modify();

	// Create the simulation stage UObject (UNiagaraSimulationStageGeneric is the standard type)
	UNiagaraSimulationStageGeneric* NewStage = NewObject<UNiagaraSimulationStageGeneric>(Emitter, NAME_None, RF_Transactional);
	NewStage->Script = NewObject<UNiagaraScript>(NewStage, MakeUniqueObjectName(NewStage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")), EObjectFlags::RF_Transactional);
	NewStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	NewStage->Script->SetUsageId(NewStage->GetMergeId());
	if (Source)
	{
		NewStage->Script->SetLatestSource(Source);
	}

	// Set optional properties
	FString IterSourceStr = Params->HasField(TEXT("iteration_source")) ? Params->GetStringField(TEXT("iteration_source")) : TEXT("particles");
	if (IterSourceStr.Equals(TEXT("data_interface"), ESearchCase::IgnoreCase))
	{
		NewStage->IterationSource = ENiagaraIterationSource::DataInterface;
	}

	// Add to emitter via exported API
	Emitter->AddSimulationStage(NewStage, VersionedEmitter.Version);

	GEditor->EndTransaction();
	System->RequestCompile(false);
	Emitter->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("stage_name"), NewStage->SimulationStageName.ToString());
	R->SetStringField(TEXT("stage_id"), NewStage->GetMergeId().ToString());
	R->SetStringField(TEXT("iteration_source"), IterSourceStr);
	return FMonolithActionResult::Success(R);
}
