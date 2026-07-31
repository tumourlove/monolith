#include "MonolithNiagaraActions.h"
#include "MonolithNiagaraLayoutActions.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
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
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraDataInterface.h"
// Tranche 2 (#64): per-system DI enumeration. Verified UE 5.7 (offline source index):
// FNiagaraDataInterfaceUtilities::ForEachDataInterface(const UNiagaraSystem*, TFunction<bool(const FDataInterfaceUsageContext&)>)
// — asset-time overload at NiagaraDataInterfaceUtilities.h:43 (NOT the FNiagaraSystemInstance* runtime overloads at :37/:40).
#include "NiagaraDataInterfaceUtilities.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraDataInterfaceVector2DCurve.h"
#include "NiagaraDataInterfaceVectorCurve.h"
#include "NiagaraDataInterfaceVector4Curve.h"
#include "NiagaraDataInterfaceGrid2DCollection.h"
#include "NiagaraDataInterfaceGrid3DCollection.h"
#include "NiagaraDataInterfaceNeighborGrid3D.h"
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
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraVolumeRendererProperties.h"
#include "NiagaraEditorModule.h"
#include "NiagaraScriptFactoryNew.h"
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
#include "NiagaraParameterCollection.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpressionDynamicParameter.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithNiagara, Log, All);

#if WITH_NIAGARA_WIZARD_PRIVATE
// Engine-PRIVATE NiagaraEditor node headers — resolvable only when WITH_NIAGARA_WIZARD_PRIVATE=1
// widens PrivateIncludePaths into NiagaraEditor/Private (see MonolithNiagara.Build.cs). The
// ParameterMap bridge below instantiates FGraphNodeCreator<> on these concrete types, which needs
// the complete definition -- a forward declaration is insufficient.
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraNodeParameterMapSet.h"

// Forward-declarations for engine-PRIVATE NiagaraEditor symbols. These are defined only in
// NiagaraEditor/Private/Widgets/DataChannel/NiagaraDataChannelWizard.cpp — there is NO public
// header. We reach the definitions by widening PrivateIncludePaths into NiagaraEditor/Private
// (see MonolithNiagara.Build.cs, gated on WITH_NIAGARA_WIZARD_PRIVATE) and forward-declaring the
// NIAGARAEDITOR_API-exported signatures here. BREADCRUMB: if a future engine bump moves/renames
// these symbols, the ParameterMap bridge in CreateScriptFromHLSL is the only consumer; flip
// WITH_NIAGARA_WIZARD_PRIVATE off (release builds already do via MONOLITH_RELEASE_BUILD) to fall
// back to the strict typed-pin path.
namespace UE::Niagara::Wizard::Utilities
{
	NIAGARAEDITOR_API UEdGraphPin* AddReadParameterPin(const FNiagaraTypeDefinition& Type, const FName& Name, UNiagaraNodeParameterMapGet* MapGetNode);
	NIAGARAEDITOR_API UEdGraphPin* AddWriteParameterPin(const FNiagaraTypeDefinition& Type, const FName& Name, UNiagaraNodeParameterMapSet* MapSetNode);
}
#endif // WITH_NIAGARA_WIZARD_PRIVATE

// ============================================================================
// Workarounds for non-exported NiagaraEditor functions
// These functions exist in NiagaraStackGraphUtilities but lack NIAGARAEDITOR_API
// ============================================================================

namespace MonolithNiagaraHelpers
{
	UEdGraphPin* GetParameterMapPin(UNiagaraNode& Node, EEdGraphPinDirection Direction);

	UClass* ResolveNiagaraDataInterfaceClass(const FString& DIClassName, FString* OutDiagnostic = nullptr)
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (DIClassName.IsEmpty())
		{
			if (OutDiagnostic) *OutDiagnostic = TEXT("Empty DI class name");
			return nullptr;
		}

		UClass* DIUClass = nullptr;
		FString Stripped = DIClassName;
		if (Stripped.StartsWith(TEXT("U")) && Stripped.Len() > 1 && FChar::IsUpper(Stripped[1]))
		{
			Stripped = Stripped.Mid(1);
		}

		TArray<FString> ExactCandidates;
		if (!Stripped.StartsWith(TEXT("NiagaraDataInterface")))
		{
			ExactCandidates.Add(TEXT("NiagaraDataInterface") + Stripped);
		}
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

		if (!DIUClass)
		{
			const FString LowerStripped = Stripped.ToLower();
			TArray<UClass*> DerivedClasses;
			GetDerivedClasses(UNiagaraDataInterface::StaticClass(), DerivedClasses, true);

			UClass* BestMatch = nullptr;
			for (UClass* DI : DerivedClasses)
			{
				if (!DI || DI->HasAnyClassFlags(CLASS_Abstract))
				{
					continue;
				}

				const FString ClassName = DI->GetName();
				const FString LowerClass = ClassName.ToLower();
				if (LowerClass == LowerStripped)
				{
					BestMatch = DI;
					break;
				}

				if (LowerClass.EndsWith(LowerStripped))
				{
					if (!BestMatch || ClassName.Len() < BestMatch->GetName().Len())
					{
						BestMatch = DI;
					}
				}
			}
			DIUClass = BestMatch;
		}

		if (!DIUClass && OutDiagnostic)
		{
			*OutDiagnostic = FString::Printf(
				TEXT("DI class not found (must be a UNiagaraDataInterface subclass). Tried exact: [%s], then fuzzy suffix scan. Input was: '%s'"),
				*FString::Join(ExactCandidates, TEXT(", ")), *DIClassName);
		}

		return DIUClass;
	}

	UNiagaraNodeOutput* ResetGraphForOutputLocal(UNiagaraGraph& NiagaraGraph, ENiagaraScriptUsage ScriptUsage,
		FGuid ScriptUsageId, const FGuid& PreferredOutputNodeGuid = FGuid(), const FGuid& PreferredInputNodeGuid = FGuid())
	{
		NiagaraGraph.Modify();
		UNiagaraNodeOutput* OutputNode = nullptr;
		for (UEdGraphNode* GraphNode : NiagaraGraph.Nodes)
		{
			UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(GraphNode);
			if (!Candidate) continue;
			if (Candidate->GetUsage() != ScriptUsage) continue;
			if (Candidate->GetUsageId() != ScriptUsageId) continue;
			OutputNode = Candidate;
			break;
		}
		UEdGraphPin* OutputNodeInputPin = OutputNode != nullptr ? GetParameterMapPin(*OutputNode, EGPD_Input) : nullptr;
		if (OutputNode != nullptr && OutputNodeInputPin == nullptr)
		{
			NiagaraGraph.RemoveNode(OutputNode);
			OutputNode = nullptr;
		}

		if (OutputNode == nullptr)
		{
			FGraphNodeCreator<UNiagaraNodeOutput> OutputNodeCreator(NiagaraGraph);
			OutputNode = OutputNodeCreator.CreateNode();
			OutputNode->SetUsage(ScriptUsage);
			OutputNode->SetUsageId(ScriptUsageId);
			OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
			OutputNodeCreator.Finalize();

			if (PreferredOutputNodeGuid.IsValid())
			{
				OutputNode->NodeGuid = PreferredOutputNodeGuid;
			}

			OutputNodeInputPin = GetParameterMapPin(*OutputNode, EGPD_Input);
		}
		else
		{
			OutputNode->Modify();
		}

		FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(NiagaraGraph);
		UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
		InputNodeCreator.Finalize();

		if (PreferredInputNodeGuid.IsValid())
		{
			InputNode->NodeGuid = PreferredInputNodeGuid;
		}

		UEdGraphPin* InputNodeOutputPin = GetParameterMapPin(*InputNode, EGPD_Output);
		if (OutputNodeInputPin)
		{
			OutputNodeInputPin->BreakAllPinLinks();
			if (InputNodeOutputPin)
			{
				OutputNodeInputPin->MakeLinkTo(InputNodeOutputPin);
			}
		}

		return OutputNode;
	}

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

	// Apply Grid2D/Grid3D/NeighborGrid3D configuration from JSON
	bool ApplyGridConfig(UNiagaraDataInterface* DI, const TSharedPtr<FJsonObject>& Config)
	{
		if (!DI || !Config.IsValid()) return false;

		if (UNiagaraDataInterfaceGrid2DCollection* Grid2D = Cast<UNiagaraDataInterfaceGrid2DCollection>(DI))
		{
			if (Config->HasField(TEXT("num_cells_x")))
			{
				Grid2D->NumCellsX = static_cast<int32>(Config->GetNumberField(TEXT("num_cells_x")));
			}
			if (Config->HasField(TEXT("num_cells_y")))
			{
				Grid2D->NumCellsY = static_cast<int32>(Config->GetNumberField(TEXT("num_cells_y")));
			}
			if (Config->HasField(TEXT("num_cells_max_axis")))
			{
				Grid2D->NumCellsMaxAxis = static_cast<int32>(Config->GetNumberField(TEXT("num_cells_max_axis")));
			}
			if (Config->HasField(TEXT("num_attributes")))
			{
				Grid2D->NumAttributes = static_cast<int32>(Config->GetNumberField(TEXT("num_attributes")));
			}
			if (Config->HasField(TEXT("world_bbox_size")))
			{
				TSharedPtr<FJsonObject> BBox = Config->GetObjectField(TEXT("world_bbox_size"));
				if (BBox.IsValid() && BBox->HasField(TEXT("x")) && BBox->HasField(TEXT("y")))
				{
					Grid2D->WorldBBoxSize.X = static_cast<float>(BBox->GetNumberField(TEXT("x")));
					Grid2D->WorldBBoxSize.Y = static_cast<float>(BBox->GetNumberField(TEXT("y")));
				}
			}
			if (Config->HasField(TEXT("set_grid_from_max_axis")))
			{
				Grid2D->SetGridFromMaxAxis = Config->GetBoolField(TEXT("set_grid_from_max_axis"));
			}
			if (Config->HasField(TEXT("clear_before_non_iteration_stage")))
			{
				Grid2D->ClearBeforeNonIterationStage = Config->GetBoolField(TEXT("clear_before_non_iteration_stage"));
			}
			return true;
		}
		else if (UNiagaraDataInterfaceGrid3DCollection* Grid3D = Cast<UNiagaraDataInterfaceGrid3DCollection>(DI))
		{
			// Grid3D uses FIntVector NumCells instead of separate X/Y/Z properties
			if (Config->HasField(TEXT("num_cells_x")) || Config->HasField(TEXT("num_cells_y")) || Config->HasField(TEXT("num_cells_z")))
			{
				int32 X = Config->HasField(TEXT("num_cells_x")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_x"))) : Grid3D->NumCells.X;
				int32 Y = Config->HasField(TEXT("num_cells_y")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_y"))) : Grid3D->NumCells.Y;
				int32 Z = Config->HasField(TEXT("num_cells_z")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_z"))) : Grid3D->NumCells.Z;
				Grid3D->NumCells = FIntVector(X, Y, Z);
			}
			if (Config->HasField(TEXT("num_cells_max_axis")))
			{
				Grid3D->NumCellsMaxAxis = static_cast<int32>(Config->GetNumberField(TEXT("num_cells_max_axis")));
			}
			if (Config->HasField(TEXT("num_attributes")))
			{
				Grid3D->NumAttributes = static_cast<int32>(Config->GetNumberField(TEXT("num_attributes")));
			}
			if (Config->HasField(TEXT("cell_size")))
			{
				Grid3D->CellSize = static_cast<float>(Config->GetNumberField(TEXT("cell_size")));
			}
			if (Config->HasField(TEXT("world_bbox_size")))
			{
				TSharedPtr<FJsonObject> BBox = Config->GetObjectField(TEXT("world_bbox_size"));
				if (BBox.IsValid() && BBox->HasField(TEXT("x")) && BBox->HasField(TEXT("y")) && BBox->HasField(TEXT("z")))
				{
					Grid3D->WorldBBoxSize.X = static_cast<float>(BBox->GetNumberField(TEXT("x")));
					Grid3D->WorldBBoxSize.Y = static_cast<float>(BBox->GetNumberField(TEXT("y")));
					Grid3D->WorldBBoxSize.Z = static_cast<float>(BBox->GetNumberField(TEXT("z")));
				}
			}
			if (Config->HasField(TEXT("set_resolution_method")))
			{
				FString Method = Config->GetStringField(TEXT("set_resolution_method"));
				if (Method.Equals(TEXT("Independent"), ESearchCase::IgnoreCase))
				{
					Grid3D->SetResolutionMethod = ESetResolutionMethod::Independent;
				}
				else if (Method.Equals(TEXT("MaxAxis"), ESearchCase::IgnoreCase))
				{
					Grid3D->SetResolutionMethod = ESetResolutionMethod::MaxAxis;
				}
				else if (Method.Equals(TEXT("CellSize"), ESearchCase::IgnoreCase))
				{
					Grid3D->SetResolutionMethod = ESetResolutionMethod::CellSize;
				}
			}
			if (Config->HasField(TEXT("clear_before_non_iteration_stage")))
			{
				Grid3D->ClearBeforeNonIterationStage = Config->GetBoolField(TEXT("clear_before_non_iteration_stage"));
			}
			return true;
		}
		else if (UNiagaraDataInterfaceNeighborGrid3D* NeighborGrid = Cast<UNiagaraDataInterfaceNeighborGrid3D>(DI))
		{
			// NeighborGrid3D inherits from Grid3D, so it also uses FIntVector NumCells
			if (Config->HasField(TEXT("num_cells_x")) || Config->HasField(TEXT("num_cells_y")) || Config->HasField(TEXT("num_cells_z")))
			{
				int32 X = Config->HasField(TEXT("num_cells_x")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_x"))) : NeighborGrid->NumCells.X;
				int32 Y = Config->HasField(TEXT("num_cells_y")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_y"))) : NeighborGrid->NumCells.Y;
				int32 Z = Config->HasField(TEXT("num_cells_z")) ? static_cast<int32>(Config->GetNumberField(TEXT("num_cells_z"))) : NeighborGrid->NumCells.Z;
				NeighborGrid->NumCells = FIntVector(X, Y, Z);
			}
			if (Config->HasField(TEXT("num_cells_max_axis")))
			{
				NeighborGrid->NumCellsMaxAxis = static_cast<int32>(Config->GetNumberField(TEXT("num_cells_max_axis")));
			}
			if (Config->HasField(TEXT("max_neighbors_per_cell")))
			{
				NeighborGrid->MaxNeighborsPerCell = static_cast<uint32>(Config->GetNumberField(TEXT("max_neighbors_per_cell")));
			}
			if (Config->HasField(TEXT("cell_size")))
			{
				NeighborGrid->CellSize = static_cast<float>(Config->GetNumberField(TEXT("cell_size")));
			}
			if (Config->HasField(TEXT("world_bbox_size")))
			{
				TSharedPtr<FJsonObject> BBox = Config->GetObjectField(TEXT("world_bbox_size"));
				if (BBox.IsValid() && BBox->HasField(TEXT("x")) && BBox->HasField(TEXT("y")) && BBox->HasField(TEXT("z")))
				{
					NeighborGrid->WorldBBoxSize.X = static_cast<float>(BBox->GetNumberField(TEXT("x")));
					NeighborGrid->WorldBBoxSize.Y = static_cast<float>(BBox->GetNumberField(TEXT("y")));
					NeighborGrid->WorldBBoxSize.Z = static_cast<float>(BBox->GetNumberField(TEXT("z")));
				}
			}
			if (Config->HasField(TEXT("set_resolution_method")))
			{
				FString Method = Config->GetStringField(TEXT("set_resolution_method"));
				if (Method.Equals(TEXT("Independent"), ESearchCase::IgnoreCase))
				{
					NeighborGrid->SetResolutionMethod = ESetResolutionMethod::Independent;
				}
				else if (Method.Equals(TEXT("MaxAxis"), ESearchCase::IgnoreCase))
				{
					NeighborGrid->SetResolutionMethod = ESetResolutionMethod::MaxAxis;
				}
				else if (Method.Equals(TEXT("CellSize"), ESearchCase::IgnoreCase))
				{
					NeighborGrid->SetResolutionMethod = ESetResolutionMethod::CellSize;
				}
			}
			if (Config->HasField(TEXT("clear_before_non_iteration_stage")))
			{
				NeighborGrid->ClearBeforeNonIterationStage = Config->GetBoolField(TEXT("clear_before_non_iteration_stage"));
			}
			return true;
		}

		return false;
	}

	// ========================================================================
	// Phase 2: Shared helpers for dynamic inputs (Phase 3) and move_module (Phase 4)
	// ========================================================================

	// Structural check: is this node a ParameterMapSet override node?
	// We can't Cast<UNiagaraNodeParameterMapSet> (private NiagaraEditor header),
	// so we identify it structurally: it's a UNiagaraNode (but NOT a FunctionCall)
	// that has ParameterMap pins on both input and output sides.
	bool IsOverrideNode(UEdGraphNode* Node)
	{
		if (!Node || Cast<UNiagaraNodeFunctionCall>(Node)) return false;
		UNiagaraNode* NN = Cast<UNiagaraNode>(Node);
		if (!NN) return false;
		UEdGraphPin* MapIn = GetParameterMapPin(*NN, EGPD_Input);
		UEdGraphPin* MapOut = GetParameterMapPin(*NN, EGPD_Output);
		return (MapIn != nullptr && MapOut != nullptr);
	}

	// Find the override node (ParameterMapSet) immediately upstream of a function call.
	// Returns nullptr if no override node exists (module has no overridden inputs).
	UEdGraphNode* GetStackFunctionOverrideNode(UNiagaraNodeFunctionCall& FuncNode)
	{
		UEdGraphPin* MapIn = GetParameterMapPin(FuncNode, EGPD_Input);
		if (!MapIn || MapIn->LinkedTo.Num() == 0) return nullptr;
		UEdGraphNode* Upstream = MapIn->LinkedTo[0]->GetOwningNode();
		return IsOverrideNode(Upstream) ? Upstream : nullptr;
	}

	// Check if a specific pin on an override node belongs to a given function call.
	// Override pins are namespaced by function name (e.g. "Initialize Particle.Lifetime Mode").
	bool IsOverridePinForFunction(UEdGraphPin* Pin, UNiagaraNodeFunctionCall& FuncNode, UEdGraphNode* OverrideNode)
	{
		if (!Pin || Pin->Direction != EGPD_Input) return false;
		// Skip the ParameterMap chain pin (structural check, not namespace constants)
		UNiagaraNode* NN = Cast<UNiagaraNode>(OverrideNode);
		if (NN && Pin == GetParameterMapPin(*NN, EGPD_Input)) return false;
		// Skip the "Add" pin (UE adds an extra pin for stack UI)
		if (Pin->PinName == TEXT("Add")) return false;
		// Match: pin namespace == function name
		FNiagaraParameterHandle Handle(Pin->PinName);
		return Handle.GetNamespace().ToString() == FuncNode.GetFunctionName();
	}

	// Collect all override pins on an override node that belong to a specific function call.
	void GetOverridePinsForFunction(UEdGraphNode* OverrideNode, UNiagaraNodeFunctionCall& FuncNode, TArray<UEdGraphPin*>& OutPins)
	{
		if (!OverrideNode) return;
		for (UEdGraphPin* Pin : OverrideNode->Pins)
		{
			if (IsOverridePinForFunction(Pin, FuncNode, OverrideNode))
			{
				OutPins.Add(Pin);
			}
		}
	}

	// A group of nodes in the stack's ParameterMap chain.
	// For a module, StartNodes are the nodes connected to the previous group's PM output
	// (typically the override node, or the function call itself if no overrides exist).
	// EndNode is the function call node whose PM output feeds the next group.
	struct FStackNodeGroup
	{
		TArray<UNiagaraNode*> StartNodes;  // Override node(s) or connector nodes feeding into this group
		UNiagaraNode* EndNode = nullptr;   // The terminal node of this group (function call or output)
	};

	// Build an ordered array of node groups from the PM chain anchored at OutputNode.
	// Group 0: the chain input connector (UNiagaraNodeInput at the start of the chain).
	// Groups 1..N: one per module, in execution order.
	// Final group: the output node itself.
	void GetStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FStackNodeGroup>& OutGroups)
	{
		OutGroups.Reset();

		// Get ordered modules via existing helper
		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		GetOrderedModuleNodes(OutputNode, ModuleNodes);

		// Group 0: walk backwards from the first module (or the output node if no modules)
		// to find the UNiagaraNodeInput that starts the PM chain.
		{
			FStackNodeGroup InputGroup;
			UNiagaraNode* FirstChainNode = ModuleNodes.Num() > 0
				? static_cast<UNiagaraNode*>(ModuleNodes[0])
				: static_cast<UNiagaraNode*>(&OutputNode);

			// Walk backwards through override nodes to find the chain start
			UNiagaraNode* Current = FirstChainNode;
			while (Current)
			{
				UEdGraphPin* MapIn = GetParameterMapPin(*Current, EGPD_Input);
				if (!MapIn || MapIn->LinkedTo.Num() == 0) break;
				UNiagaraNode* Prev = Cast<UNiagaraNode>(MapIn->LinkedTo[0]->GetOwningNode());
				if (!Prev) break;
				// If we hit the override node for the first module, keep walking
				if (IsOverrideNode(Prev))
				{
					Current = Prev;
					continue;
				}
				// Otherwise we found the input connector
				Current = Prev;
				break;
			}
			InputGroup.EndNode = Current;
			InputGroup.StartNodes.Add(Current);
			OutGroups.Add(MoveTemp(InputGroup));
		}

		// Groups 1..N: one per module
		for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
		{
			FStackNodeGroup ModuleGroup;
			ModuleGroup.EndNode = ModuleNode;

			// Check if this module has an override node upstream
			UEdGraphNode* OverrideNode = GetStackFunctionOverrideNode(*ModuleNode);
			if (OverrideNode)
			{
				ModuleGroup.StartNodes.Add(CastChecked<UNiagaraNode>(OverrideNode));
			}
			else
			{
				ModuleGroup.StartNodes.Add(ModuleNode);
			}
			OutGroups.Add(MoveTemp(ModuleGroup));
		}

		// Final group: the output node
		{
			FStackNodeGroup OutputGroup;
			OutputGroup.EndNode = &OutputNode;
			OutputGroup.StartNodes.Add(&OutputNode);
			OutGroups.Add(MoveTemp(OutputGroup));
		}
	}

	// Disconnect a group from the PM chain, reconnecting its neighbors to each other.
	// PrevGroup.EndNode output -> NextGroup.StartNodes input (bypassing Group).
	void DisconnectGroup(const FStackNodeGroup& Group, const FStackNodeGroup& PrevGroup, const FStackNodeGroup& NextGroup)
	{
		if (!PrevGroup.EndNode || !Group.EndNode) return;

		// Break PM output links from PrevGroup.EndNode
		UEdGraphPin* PrevMapOut = GetParameterMapPin(*PrevGroup.EndNode, EGPD_Output);
		if (PrevMapOut)
		{
			PrevMapOut->BreakAllPinLinks();
		}

		// Break PM output links from Group.EndNode
		UEdGraphPin* GroupMapOut = GetParameterMapPin(*Group.EndNode, EGPD_Output);
		if (GroupMapOut)
		{
			GroupMapOut->BreakAllPinLinks();
		}

		// Also break PM input links on Group.StartNodes (detach from prev)
		for (UNiagaraNode* StartNode : Group.StartNodes)
		{
			if (StartNode)
			{
				UEdGraphPin* StartMapIn = GetParameterMapPin(*StartNode, EGPD_Input);
				if (StartMapIn)
				{
					StartMapIn->BreakAllPinLinks();
				}
			}
		}

		// Reconnect: PrevGroup.EndNode -> NextGroup.StartNodes
		if (PrevMapOut)
		{
			for (UNiagaraNode* NextStart : NextGroup.StartNodes)
			{
				if (NextStart)
				{
					UEdGraphPin* NextMapIn = GetParameterMapPin(*NextStart, EGPD_Input);
					if (NextMapIn)
					{
						PrevMapOut->MakeLinkTo(NextMapIn);
					}
				}
			}
		}
	}

	// Insert a group into the PM chain between NewPrevGroup and NewNextGroup.
	// Breaks NewPrevGroup.EndNode -> NewNextGroup link, then wires:
	//   NewPrevGroup.EndNode -> Group.StartNodes
	//   Group.EndNode -> NewNextGroup.StartNodes
	void ConnectGroup(const FStackNodeGroup& Group, const FStackNodeGroup& NewPrevGroup, const FStackNodeGroup& NewNextGroup)
	{
		if (!NewPrevGroup.EndNode || !Group.EndNode) return;

		// Break the existing link from NewPrevGroup -> NewNextGroup
		UEdGraphPin* PrevMapOut = GetParameterMapPin(*NewPrevGroup.EndNode, EGPD_Output);
		if (PrevMapOut)
		{
			PrevMapOut->BreakAllPinLinks();
		}

		// Wire NewPrevGroup.EndNode -> Group.StartNodes
		if (PrevMapOut)
		{
			for (UNiagaraNode* StartNode : Group.StartNodes)
			{
				if (StartNode)
				{
					UEdGraphPin* StartMapIn = GetParameterMapPin(*StartNode, EGPD_Input);
					if (StartMapIn)
					{
						PrevMapOut->MakeLinkTo(StartMapIn);
					}
				}
			}
		}

		// Wire Group.EndNode -> NewNextGroup.StartNodes
		UEdGraphPin* GroupMapOut = GetParameterMapPin(*Group.EndNode, EGPD_Output);
		if (GroupMapOut)
		{
			for (UNiagaraNode* NextStart : NewNextGroup.StartNodes)
			{
				if (NextStart)
				{
					UEdGraphPin* NextMapIn = GetParameterMapPin(*NextStart, EGPD_Input);
					if (NextMapIn)
					{
						GroupMapOut->MakeLinkTo(NextMapIn);
					}
				}
			}
		}
	}

} // namespace MonolithNiagaraHelpers

// Helper: wrap a string result in a FJsonObject for FMonolithActionResult::Success
static FMonolithActionResult NA_SuccessStr(const FString& Msg)
{
	TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("result"), Msg);
	return FMonolithActionResult::Success(R);
}

// Helper: wrap a pre-built JSON object for Success
static FMonolithActionResult NA_SuccessObj(const TSharedRef<FJsonObject>& Obj)
{
	return FMonolithActionResult::Success(Obj);
}

// Helper: normalize asset path parameter — accepts "asset_path" (preferred) with "system_path" fallback
static FString NA_GetAssetPath(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = Params->GetStringField(TEXT("asset_path"));
	if (Path.IsEmpty()) Path = Params->GetStringField(TEXT("system_path"));
	return Path;
}

namespace
{
	enum class EMonolithSemanticDetailLevel
	{
		Compact,
		Full
	};

	static bool TryParseSemanticDetailLevel(
		const TSharedPtr<FJsonObject>& Params,
		EMonolithSemanticDetailLevel& OutDetailLevel,
		FString& OutError)
	{
		const FString DetailLevel = Params->HasField(TEXT("detail_level"))
			? Params->GetStringField(TEXT("detail_level")).ToLower()
			: TEXT("compact");

		if (DetailLevel.IsEmpty() || DetailLevel == TEXT("compact"))
		{
			OutDetailLevel = EMonolithSemanticDetailLevel::Compact;
			return true;
		}

		if (DetailLevel == TEXT("full"))
		{
			OutDetailLevel = EMonolithSemanticDetailLevel::Full;
			return true;
		}

		OutError = FString::Printf(TEXT("Invalid detail_level '%s'. Valid values: compact, full"), *DetailLevel);
		return false;
	}

	struct FMonolithNiagaraStageModule
	{
		FString StageName;
		FString ModuleName;
		FString ModuleGuid;
	};

	struct FMonolithNiagaraEventGeneratorInfo
	{
		FString EventName;
		FString ModuleName;
		FString ModuleGuid;
		FString StageName;
	};

	struct FMonolithNiagaraTopologyEdge
	{
		FString SourceEmitterId;
		FString SourceEmitterName;
		FString TargetEmitterId;
		FString TargetEmitterName;
		FString EventName;
		FString ExecutionMode;
		int32 SpawnNumber = 0;
		int32 MaxEventsPerFrame = 0;
		bool bRandomSpawnNumber = false;
		int32 MinSpawnNumber = 0;
		bool bUpdateAttributeInitialValues = false;
		FString UsageId;
		bool bSourceEmitterResolved = false;
	};

	struct FMonolithNiagaraEmitterSemantic
	{
		FString Name;
		FString Guid;
		int32 Index = INDEX_NONE;
		bool bEnabled = false;
		bool bLocalSpace = false;
		bool bDeterminism = false;
		bool bRequiresPersistentIDs = false;
		FString CalculateBoundsMode;
		bool bHasSpriteRenderer = false;
		bool bHasRibbonRenderer = false;
		bool bHasMeshRenderer = false;
		bool bHasLocalSpawnLocationModule = false;
		bool bHasAnyLocationModule = false;
		FString SpawnLocationMode;
		FString RoleHint;
		TArray<FMonolithNiagaraStageModule> LocationModules;
		TArray<FMonolithNiagaraEventGeneratorInfo> EventGenerators;
		TArray<FMonolithNiagaraTopologyEdge> IncomingEvents;
		TArray<FMonolithNiagaraTopologyEdge> OutgoingLinks;
		TArray<FString> SemanticNotes;
	};

	static void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty() && !Values.Contains(Value))
		{
			Values.Add(Value);
		}
	}

	static FString BoundsModeToString(const ENiagaraEmitterCalculateBoundMode Mode)
	{
		switch (Mode)
		{
		case ENiagaraEmitterCalculateBoundMode::Dynamic: return TEXT("Dynamic");
		case ENiagaraEmitterCalculateBoundMode::Fixed: return TEXT("Fixed");
		case ENiagaraEmitterCalculateBoundMode::Programmable: return TEXT("Programmable");
		default: return TEXT("Unknown");
		}
	}

	static FString ExecutionModeToString(const EScriptExecutionMode Mode)
	{
		switch (Mode)
		{
		case EScriptExecutionMode::EveryParticle: return TEXT("EveryParticle");
		case EScriptExecutionMode::SpawnedParticles: return TEXT("SpawnedParticles");
		case EScriptExecutionMode::SingleParticle: return TEXT("SingleParticle");
		default: return TEXT("Unknown");
		}
	}

	static FString CanonicalizeEventName(const FString& EventName)
	{
		const FString Lower = EventName.ToLower();
		if (Lower.Contains(TEXT("death"))) return TEXT("DeathEvent");
		if (Lower.Contains(TEXT("location"))) return TEXT("LocationEvent");
		if (Lower.Contains(TEXT("collision"))) return TEXT("CollisionEvent");
		return EventName;
	}

	static bool TryGetGeneratedEventName(const FString& ModuleName, FString& OutEventName)
	{
		const FString Lower = ModuleName.ToLower();
		if (Lower.Contains(TEXT("generatedeathevent")))
		{
			OutEventName = TEXT("DeathEvent");
			return true;
		}
		if (Lower.Contains(TEXT("generatelocationevent")))
		{
			OutEventName = TEXT("LocationEvent");
			return true;
		}
		if (Lower.Contains(TEXT("generatecollisionevent")))
		{
			OutEventName = TEXT("CollisionEvent");
			return true;
		}
		return false;
	}

	static bool IsLocalSpawnLocationModule(const FString& ModuleName)
	{
		const FString Lower = ModuleName.ToLower();
		if (!Lower.Contains(TEXT("location")))
		{
			return false;
		}
		if (Lower.Contains(TEXT("event")))
		{
			return false;
		}
		return true;
	}

	static bool NameSuggestsBurst(const FString& LowerName)
	{
		return LowerName.Contains(TEXT("burst"))
			|| LowerName.Contains(TEXT("explosion"))
			|| LowerName.Contains(TEXT("explode"))
			|| LowerName.Contains(TEXT("impact"));
	}

	static bool NameSuggestsTrail(const FString& LowerName)
	{
		return LowerName.Contains(TEXT("trail")) || LowerName.Contains(TEXT("ribbon"));
	}

	static bool NameSuggestsShell(const FString& LowerName)
	{
		return LowerName.Contains(TEXT("shell"))
			|| LowerName.Contains(TEXT("leader"))
			|| LowerName.Contains(TEXT("rocket"))
			|| LowerName.Contains(TEXT("projectile"));
	}

	// Emitter-stage usages only; system-stage branch dropped as dead code (no caller passes
	// SystemSpawnScript/SystemUpdateScript — see CollectEmitterModules below).
	static UNiagaraGraph* GetGraphForHandleUsage(UNiagaraSystem* System, const FNiagaraEmitterHandle& Handle, const ENiagaraScriptUsage /*Usage*/)
	{
		if (!System)
		{
			return nullptr;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		UNiagaraScriptSource* Source = EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr;
		return Source ? Source->NodeGraph : nullptr;
	}

	static void CollectEmitterModules(UNiagaraSystem* System, const FNiagaraEmitterHandle& Handle, TArray<FMonolithNiagaraStageModule>& OutModules)
	{
		OutModules.Reset();

		static const TPair<ENiagaraScriptUsage, const TCHAR*> StageUsages[] = {
			{ENiagaraScriptUsage::EmitterSpawnScript, TEXT("emitter_spawn")},
			{ENiagaraScriptUsage::EmitterUpdateScript, TEXT("emitter_update")},
			{ENiagaraScriptUsage::ParticleSpawnScript, TEXT("particle_spawn")},
			{ENiagaraScriptUsage::ParticleUpdateScript, TEXT("particle_update")},
		};

		for (const auto& [Usage, StageName] : StageUsages)
		{
			UNiagaraGraph* Graph = GetGraphForHandleUsage(System, Handle, Usage);
			UNiagaraNodeOutput* OutputNode = Graph ? Graph->FindEquivalentOutputNode(Usage, FGuid()) : nullptr;
			if (!OutputNode)
			{
				continue;
			}

			TArray<UNiagaraNodeFunctionCall*> Modules;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, Modules);
			for (UNiagaraNodeFunctionCall* ModuleNode : Modules)
			{
				if (!ModuleNode)
				{
					continue;
				}

				FMonolithNiagaraStageModule Module;
				Module.StageName = StageName;
				Module.ModuleName = ModuleNode->GetFunctionName();
				Module.ModuleGuid = ModuleNode->NodeGuid.ToString();
				OutModules.Add(MoveTemp(Module));
			}
		}
	}

	static void CollectTopologyEdges(UNiagaraSystem* System, TArray<FMonolithNiagaraTopologyEdge>& OutEdges)
	{
		OutEdges.Reset();
		if (!System)
		{
			return;
		}

		const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		for (const FNiagaraEmitterHandle& TargetHandle : Handles)
		{
			FVersionedNiagaraEmitterData* EmitterData = TargetHandle.GetEmitterData();
			if (!EmitterData)
			{
				continue;
			}

			for (const FNiagaraEventScriptProperties& EventProps : EmitterData->GetEventHandlers())
			{
				FMonolithNiagaraTopologyEdge Edge;
				Edge.TargetEmitterId = TargetHandle.GetId().ToString();
				Edge.TargetEmitterName = TargetHandle.GetName().ToString();
				Edge.SourceEmitterId = EventProps.SourceEmitterID.ToString();
				Edge.EventName = CanonicalizeEventName(EventProps.SourceEventName.ToString());
				Edge.ExecutionMode = ExecutionModeToString(EventProps.ExecutionMode);
				Edge.SpawnNumber = static_cast<int32>(EventProps.SpawnNumber);
				Edge.MaxEventsPerFrame = static_cast<int32>(EventProps.MaxEventsPerFrame);
				Edge.bRandomSpawnNumber = EventProps.bRandomSpawnNumber;
				Edge.MinSpawnNumber = static_cast<int32>(EventProps.MinSpawnNumber);
				Edge.bUpdateAttributeInitialValues = EventProps.UpdateAttributeInitialValues;
				if (EventProps.Script)
				{
					Edge.UsageId = EventProps.Script->GetUsageId().ToString();
				}

				if (EventProps.SourceEmitterID.IsValid())
				{
					for (const FNiagaraEmitterHandle& SourceHandle : Handles)
					{
						if (SourceHandle.GetId() == EventProps.SourceEmitterID)
						{
							Edge.SourceEmitterName = SourceHandle.GetName().ToString();
							Edge.bSourceEmitterResolved = true;
							break;
						}
					}
				}

				OutEdges.Add(MoveTemp(Edge));
			}
		}
	}

	static FMonolithNiagaraEmitterSemantic AnalyzeEmitterSemantic(
		UNiagaraSystem* System,
		const FNiagaraEmitterHandle& Handle,
		const int32 EmitterIndex,
		const TArray<FMonolithNiagaraTopologyEdge>& TopologyEdges)
	{
		FMonolithNiagaraEmitterSemantic Semantic;
		Semantic.Name = Handle.GetName().ToString();
		Semantic.Guid = Handle.GetId().ToString();
		Semantic.Index = EmitterIndex;
		Semantic.bEnabled = Handle.GetIsEnabled();

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (EmitterData)
		{
			Semantic.bLocalSpace = EmitterData->bLocalSpace != 0;
			Semantic.bDeterminism = EmitterData->bDeterminism != 0;
			Semantic.bRequiresPersistentIDs = EmitterData->bRequiresPersistentIDs != 0;
			Semantic.CalculateBoundsMode = BoundsModeToString(EmitterData->CalculateBoundsMode);

			for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
			{
				Semantic.bHasSpriteRenderer |= Renderer && Renderer->IsA<UNiagaraSpriteRendererProperties>();
				Semantic.bHasRibbonRenderer |= Renderer && Renderer->IsA<UNiagaraRibbonRendererProperties>();
				Semantic.bHasMeshRenderer |= Renderer && Renderer->IsA<UNiagaraMeshRendererProperties>();
			}
		}

		TArray<FMonolithNiagaraStageModule> Modules;
		CollectEmitterModules(System, Handle, Modules);
		for (const FMonolithNiagaraStageModule& Module : Modules)
		{
			FString EventName;
			if (TryGetGeneratedEventName(Module.ModuleName, EventName))
			{
				FMonolithNiagaraEventGeneratorInfo Generator;
				Generator.EventName = EventName;
				Generator.ModuleName = Module.ModuleName;
				Generator.ModuleGuid = Module.ModuleGuid;
				Generator.StageName = Module.StageName;
				Semantic.EventGenerators.Add(MoveTemp(Generator));
			}

			if (IsLocalSpawnLocationModule(Module.ModuleName))
			{
				Semantic.bHasAnyLocationModule = true;
				if (Module.StageName == TEXT("particle_spawn"))
				{
					Semantic.bHasLocalSpawnLocationModule = true;
				}

				Semantic.LocationModules.Add(Module);
			}
		}

		for (const FMonolithNiagaraTopologyEdge& Edge : TopologyEdges)
		{
			if (Edge.TargetEmitterId == Semantic.Guid)
			{
				Semantic.IncomingEvents.Add(Edge);
			}
			if (Edge.SourceEmitterId == Semantic.Guid)
			{
				Semantic.OutgoingLinks.Add(Edge);
			}
		}

		if (Semantic.IncomingEvents.Num() > 0 && Semantic.bHasLocalSpawnLocationModule)
		{
			Semantic.SpawnLocationMode = TEXT("mixed_event_and_local_shape");
		}
		else if (Semantic.IncomingEvents.Num() > 0)
		{
			Semantic.SpawnLocationMode = TEXT("event_driven");
		}
		else if (Semantic.bHasLocalSpawnLocationModule)
		{
			Semantic.SpawnLocationMode = TEXT("local_shape");
		}
		else if (Semantic.bHasAnyLocationModule)
		{
			Semantic.SpawnLocationMode = TEXT("module_location");
		}
		else
		{
			Semantic.SpawnLocationMode = TEXT("default_or_unknown");
		}

		const FString LowerName = Semantic.Name.ToLower();
		if (NameSuggestsTrail(LowerName) && Semantic.IncomingEvents.Num() > 0)
		{
			Semantic.RoleHint = TEXT("trail_follower");
		}
		else if (NameSuggestsShell(LowerName) && Semantic.EventGenerators.Num() > 0)
		{
			Semantic.RoleHint = TEXT("shell_event_source");
		}
		else if (NameSuggestsBurst(LowerName) && Semantic.IncomingEvents.Num() > 0)
		{
			Semantic.RoleHint = TEXT("burst_receiver");
		}
		else if (NameSuggestsBurst(LowerName)
			&& (Semantic.SpawnLocationMode == TEXT("local_shape")
				|| Semantic.SpawnLocationMode == TEXT("module_location")
				|| Semantic.SpawnLocationMode == TEXT("default_or_unknown")))
		{
			Semantic.RoleHint = TEXT("independent_burst");
		}
		else if (Semantic.IncomingEvents.Num() > 0)
		{
			Semantic.RoleHint = TEXT("event_receiver");
		}
		else if (Semantic.EventGenerators.Num() > 0)
		{
			Semantic.RoleHint = TEXT("event_source");
		}
		else if (NameSuggestsTrail(LowerName))
		{
			Semantic.RoleHint = TEXT("trail_or_ribbon");
		}
		else
		{
			Semantic.RoleHint = TEXT("independent_emitter");
		}

		for (const FMonolithNiagaraTopologyEdge& Edge : Semantic.IncomingEvents)
		{
			const FString SourceName = Edge.SourceEmitterName.IsEmpty() ? TEXT("<unresolved>") : Edge.SourceEmitterName;
			Semantic.SemanticNotes.Add(FString::Printf(TEXT("Consumes %s from %s."), *Edge.EventName, *SourceName));
		}

		TArray<FString> GeneratedEvents;
		for (const FMonolithNiagaraEventGeneratorInfo& Generator : Semantic.EventGenerators)
		{
			AddUniqueString(GeneratedEvents, Generator.EventName);
		}
		for (const FString& EventName : GeneratedEvents)
		{
			Semantic.SemanticNotes.Add(FString::Printf(TEXT("Generates %s for downstream emitters."), *EventName));
		}

		if (Semantic.SpawnLocationMode == TEXT("local_shape"))
		{
			Semantic.SemanticNotes.Add(TEXT("Spawns from local shape/location modules, so without an incoming event it appears at its own origin or shape."));
		}
		else if (Semantic.SpawnLocationMode == TEXT("event_driven"))
		{
			Semantic.SemanticNotes.Add(TEXT("Spawn placement is event-driven rather than local-shape-driven."));
		}
		else if (Semantic.SpawnLocationMode == TEXT("mixed_event_and_local_shape"))
		{
			Semantic.SemanticNotes.Add(TEXT("Consumes incoming events but also has local location modules; verify whether placement should be event-driven or independent."));
		}

		if (Semantic.RoleHint == TEXT("independent_burst"))
		{
			Semantic.SemanticNotes.Add(TEXT("Name suggests a burst/explosion, but no incoming event handler exists; it behaves as an independent burst."));
		}

		if ((Semantic.EventGenerators.Num() > 0 || Semantic.IncomingEvents.Num() > 0) && !Semantic.bRequiresPersistentIDs)
		{
			Semantic.SemanticNotes.Add(TEXT("Participates in inter-emitter event flow while requires_persistent_ids is false; verify whether stable source particle identity is needed."));
		}

		return Semantic;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TSharedRef<FJsonObject> MakeStageModuleJson(const FMonolithNiagaraStageModule& Module)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("module_name"), Module.ModuleName);
		Obj->SetStringField(TEXT("module_guid"), Module.ModuleGuid);
		Obj->SetStringField(TEXT("stage"), Module.StageName);
		return Obj;
	}

	static TSharedRef<FJsonObject> MakeEventGeneratorJson(const FMonolithNiagaraEventGeneratorInfo& Generator)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("event_name"), Generator.EventName);
		Obj->SetStringField(TEXT("module_name"), Generator.ModuleName);
		Obj->SetStringField(TEXT("module_guid"), Generator.ModuleGuid);
		Obj->SetStringField(TEXT("stage"), Generator.StageName);
		return Obj;
	}

	static TSharedRef<FJsonObject> MakeTopologyEdgeJson(
		const FMonolithNiagaraTopologyEdge& Edge,
		const EMonolithSemanticDetailLevel DetailLevel)
	{
		const bool bFull = DetailLevel == EMonolithSemanticDetailLevel::Full;
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("source_emitter_name"), Edge.SourceEmitterName);
		Obj->SetStringField(TEXT("target_emitter_name"), Edge.TargetEmitterName);
		Obj->SetStringField(TEXT("event_name"), Edge.EventName);
		Obj->SetStringField(TEXT("execution_mode"), Edge.ExecutionMode);
		Obj->SetBoolField(TEXT("source_emitter_resolved"), Edge.bSourceEmitterResolved);

		if (bFull || !Edge.bSourceEmitterResolved)
		{
			Obj->SetStringField(TEXT("source_emitter_id"), Edge.SourceEmitterId);
		}

		if (bFull)
		{
			Obj->SetStringField(TEXT("target_emitter_id"), Edge.TargetEmitterId);
			Obj->SetNumberField(TEXT("spawn_number"), Edge.SpawnNumber);
			Obj->SetNumberField(TEXT("max_events_per_frame"), Edge.MaxEventsPerFrame);
			Obj->SetBoolField(TEXT("random_spawn_number"), Edge.bRandomSpawnNumber);
			Obj->SetNumberField(TEXT("min_spawn_number"), Edge.MinSpawnNumber);
			Obj->SetBoolField(TEXT("update_attribute_initial_values"), Edge.bUpdateAttributeInitialValues);
			Obj->SetStringField(TEXT("usage_id"), Edge.UsageId);
		}

		return Obj;
	}

	static void AppendEmitterSemanticJson(
		TSharedRef<FJsonObject> Obj,
		const FMonolithNiagaraEmitterSemantic& Semantic,
		const EMonolithSemanticDetailLevel DetailLevel)
	{
		const bool bFull = DetailLevel == EMonolithSemanticDetailLevel::Full;
		Obj->SetStringField(TEXT("guid"), Semantic.Guid);
		Obj->SetNumberField(TEXT("index"), Semantic.Index);
		Obj->SetBoolField(TEXT("enabled"), Semantic.bEnabled);
		Obj->SetBoolField(TEXT("requires_persistent_ids"), Semantic.bRequiresPersistentIDs);
		Obj->SetStringField(TEXT("calculate_bounds_mode"), Semantic.CalculateBoundsMode);
		Obj->SetStringField(TEXT("spawn_location_mode"), Semantic.SpawnLocationMode);
		Obj->SetStringField(TEXT("role_hint"), Semantic.RoleHint);
		Obj->SetBoolField(TEXT("has_local_spawn_location_module"), Semantic.bHasLocalSpawnLocationModule);
		Obj->SetBoolField(TEXT("has_any_location_module"), Semantic.bHasAnyLocationModule);
		Obj->SetBoolField(TEXT("has_sprite_renderer"), Semantic.bHasSpriteRenderer);
		Obj->SetBoolField(TEXT("has_ribbon_renderer"), Semantic.bHasRibbonRenderer);
		Obj->SetBoolField(TEXT("has_mesh_renderer"), Semantic.bHasMeshRenderer);
		Obj->SetNumberField(TEXT("incoming_event_count"), Semantic.IncomingEvents.Num());
		Obj->SetNumberField(TEXT("outgoing_link_count"), Semantic.OutgoingLinks.Num());
		Obj->SetNumberField(TEXT("event_generator_count"), Semantic.EventGenerators.Num());
		Obj->SetNumberField(TEXT("location_module_count"), Semantic.LocationModules.Num());

		TArray<FString> GeneratedEvents;
		for (const FMonolithNiagaraEventGeneratorInfo& Generator : Semantic.EventGenerators)
		{
			AddUniqueString(GeneratedEvents, Generator.EventName);
		}
		Obj->SetArrayField(TEXT("generated_events"), BuildStringArray(GeneratedEvents));

		TArray<FString> ConsumedEvents;
		for (const FMonolithNiagaraTopologyEdge& Incoming : Semantic.IncomingEvents)
		{
			AddUniqueString(ConsumedEvents, Incoming.EventName);
		}
		Obj->SetArrayField(TEXT("consumed_events"), BuildStringArray(ConsumedEvents));

		if (!bFull)
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> EventGeneratorsArr;
		for (const FMonolithNiagaraEventGeneratorInfo& Generator : Semantic.EventGenerators)
		{
			EventGeneratorsArr.Add(MakeShared<FJsonValueObject>(MakeEventGeneratorJson(Generator)));
		}
		Obj->SetArrayField(TEXT("event_generators"), EventGeneratorsArr);

		TArray<TSharedPtr<FJsonValue>> IncomingEventsArr;
		for (const FMonolithNiagaraTopologyEdge& Incoming : Semantic.IncomingEvents)
		{
			IncomingEventsArr.Add(MakeShared<FJsonValueObject>(MakeTopologyEdgeJson(Incoming, DetailLevel)));
		}
		Obj->SetArrayField(TEXT("incoming_events"), IncomingEventsArr);

		TArray<TSharedPtr<FJsonValue>> OutgoingLinksArr;
		for (const FMonolithNiagaraTopologyEdge& Outgoing : Semantic.OutgoingLinks)
		{
			OutgoingLinksArr.Add(MakeShared<FJsonValueObject>(MakeTopologyEdgeJson(Outgoing, DetailLevel)));
		}
		Obj->SetArrayField(TEXT("outgoing_links"), OutgoingLinksArr);

		TArray<TSharedPtr<FJsonValue>> LocationModulesArr;
		for (const FMonolithNiagaraStageModule& Module : Semantic.LocationModules)
		{
			LocationModulesArr.Add(MakeShared<FJsonValueObject>(MakeStageModuleJson(Module)));
		}
		Obj->SetArrayField(TEXT("location_modules"), LocationModulesArr);
		Obj->SetArrayField(TEXT("semantic_notes"), BuildStringArray(Semantic.SemanticNotes));
	}
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

UEnum* FMonolithNiagaraActions::TryGetStaticSwitchEnum(UEdGraphPin* SwitchPin, UNiagaraNodeFunctionCall* ModuleNode)
{
	if (!SwitchPin) return nullptr;

	if (SwitchPin->PinType.PinSubCategoryObject.IsValid())
	{
		if (UEnum* Enum = Cast<UEnum>(SwitchPin->PinType.PinSubCategoryObject.Get()))
		{
			return Enum;
		}
	}

	if (!ModuleNode) return nullptr;
	UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
	if (!CalledGraph) return nullptr;

	for (UEdGraphNode* Node : CalledGraph->Nodes)
	{
		UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
		if (!InputNode) continue;
		if (InputNode->Input.GetName() != SwitchPin->GetFName()) continue;

		if (InputNode->Input.GetType().GetEnum())
		{
			return InputNode->Input.GetType().GetEnum();
		}
	}

	return nullptr;
}

bool FMonolithNiagaraActions::ResolveStaticSwitchEnumValue(UEnum* Enum, const FString& RequestedValue, FString& OutRawValue, FString* OutDisplayValue)
{
	OutRawValue = RequestedValue;
	if (OutDisplayValue) OutDisplayValue->Reset();
	if (!Enum) return false;

	int64 EnumValue = Enum->GetValueByNameString(RequestedValue, EGetByNameFlags::CaseSensitive);
	if (EnumValue == INDEX_NONE)
	{
		EnumValue = Enum->GetValueByNameString(RequestedValue, EGetByNameFlags::None);
	}

	if (EnumValue == INDEX_NONE)
	{
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const int64 CandidateValue = Enum->GetValueByIndex(Index);
			if (!Enum->IsValidEnumValue(CandidateValue)) continue;
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			if (DisplayName.Equals(RequestedValue, ESearchCase::IgnoreCase))
			{
				EnumValue = CandidateValue;
				break;
			}
		}
	}

	if (EnumValue == INDEX_NONE)
	{
		return false;
	}

	OutRawValue = Enum->GetNameStringByValue(EnumValue);
	if (OutDisplayValue)
	{
		*OutDisplayValue = Enum->GetDisplayNameTextByValue(EnumValue).ToString();
	}
	return true;
}

void FMonolithNiagaraActions::AddStaticSwitchEnumMetadata(TSharedRef<FJsonObject> JsonObj, UEnum* Enum, const FString& RawValue)
{
	if (!Enum) return;

	JsonObj->SetStringField(TEXT("enum_name"), Enum->GetName());

	int64 EnumValue = Enum->GetValueByNameString(RawValue, EGetByNameFlags::CaseSensitive);
	if (EnumValue == INDEX_NONE)
	{
		EnumValue = Enum->GetValueByNameString(RawValue, EGetByNameFlags::None);
	}

	if (EnumValue != INDEX_NONE)
	{
		JsonObj->SetStringField(TEXT("display_value"), Enum->GetDisplayNameTextByValue(EnumValue).ToString());
	}

	TArray<TSharedPtr<FJsonValue>> Options;
	for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
	{
		const int64 OptionValue = Enum->GetValueByIndex(Index);
		if (!Enum->IsValidEnumValue(OptionValue)) continue;

		TSharedRef<FJsonObject> OptionObj = MakeShared<FJsonObject>();
		OptionObj->SetStringField(TEXT("raw_value"), Enum->GetNameStringByValue(OptionValue));
		OptionObj->SetStringField(TEXT("display_value"), Enum->GetDisplayNameTextByIndex(Index).ToString());
		Options.Add(MakeShared<FJsonValueObject>(OptionObj));
	}
	JsonObj->SetArrayField(TEXT("valid_options"), Options);
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
	if (L == TEXT("particle_event") || L == TEXT("particleevent") || L == TEXT("event")) { OutUsage = ENiagaraScriptUsage::ParticleEventScript; return true; }
	if (L == TEXT("particle_simulation_stage") || L == TEXT("particle_sim_stage") || L == TEXT("simulation_stage")
		|| L == TEXT("sim_stage") || L == TEXT("stage"))
	{
		OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
		return true;
	}
	return false;
}

bool FMonolithNiagaraActions::IsSimulationStageUsageString(const FString& UsageString)
{
	ENiagaraScriptUsage Usage;
	return ResolveScriptUsage(UsageString, Usage)
		&& Usage == ENiagaraScriptUsage::ParticleSimulationStageScript;
}

bool FMonolithNiagaraActions::IsParticleEventUsageString(const FString& UsageString)
{
	ENiagaraScriptUsage Usage;
	return ResolveScriptUsage(UsageString, Usage)
		&& Usage == ENiagaraScriptUsage::ParticleEventScript;
}

bool FMonolithNiagaraActions::ResolveSimulationStageSelector(UNiagaraSystem* System, const FString& EmitterHandleId,
	const TSharedPtr<FJsonObject>& Params, FGuid& OutUsageId, FString* OutStageName, FString* OutError)
{
	OutUsageId.Invalidate();
	if (OutStageName) OutStageName->Reset();
	if (OutError) OutError->Reset();

	if (!System)
	{
		if (OutError) *OutError = TEXT("System is null");
		return false;
	}

	const int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx == INDEX_NONE)
	{
		if (OutError) *OutError = FString::Printf(TEXT("Emitter '%s' not found"), *EmitterHandleId);
		return false;
	}

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EmitterIdx].GetEmitterData();
	if (!ED)
	{
		if (OutError) *OutError = TEXT("No emitter data");
		return false;
	}

	const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
	if (Stages.Num() == 0)
	{
		if (OutError) *OutError = TEXT("Emitter has no simulation stages");
		return false;
	}

	const FString UsageIdStr = Params->HasField(TEXT("usage_id")) ? Params->GetStringField(TEXT("usage_id")) : FString();
	const FString StageName = Params->HasField(TEXT("stage_name")) ? Params->GetStringField(TEXT("stage_name")) : FString();
	const int32 StageIndex = Params->HasField(TEXT("stage_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("stage_index"))) : INDEX_NONE;

	UNiagaraSimulationStageBase* TargetStage = nullptr;
	if (!UsageIdStr.IsEmpty())
	{
		FGuid UsageId;
		if (!FGuid::Parse(UsageIdStr, UsageId))
		{
			if (OutError) *OutError = FString::Printf(TEXT("Invalid usage_id GUID '%s'"), *UsageIdStr);
			return false;
		}

		for (UNiagaraSimulationStageBase* Stage : Stages)
		{
			if (Stage && Stage->Script && Stage->Script->GetUsageId() == UsageId)
			{
				TargetStage = Stage;
				break;
			}
		}

		if (!TargetStage)
		{
			if (OutError) *OutError = FString::Printf(TEXT("Simulation stage usage_id '%s' not found"), *UsageIdStr);
			return false;
		}
	}
	else if (!StageName.IsEmpty())
	{
		const FName TargetName(*StageName);
		for (UNiagaraSimulationStageBase* Stage : Stages)
		{
			if (Stage && Stage->SimulationStageName == TargetName)
			{
				TargetStage = Stage;
				break;
			}
		}

		if (!TargetStage)
		{
			if (OutError) *OutError = FString::Printf(TEXT("Simulation stage '%s' not found"), *StageName);
			return false;
		}
	}
	else if (StageIndex != INDEX_NONE)
	{
		if (!Stages.IsValidIndex(StageIndex))
		{
			if (OutError) *OutError = FString::Printf(TEXT("Stage index %d out of range (0-%d)"), StageIndex, Stages.Num() - 1);
			return false;
		}
		TargetStage = Stages[StageIndex];
	}
	else if (Stages.Num() == 1)
	{
		TargetStage = Stages[0];
	}
	else
	{
		if (OutError)
		{
			TArray<FString> StageLabels;
			for (UNiagaraSimulationStageBase* Stage : Stages)
			{
				if (!Stage) continue;
				FString Label = Stage->SimulationStageName.ToString();
				if (Stage->Script)
				{
					Label += FString::Printf(TEXT(" (%s)"), *Stage->Script->GetUsageId().ToString());
				}
				StageLabels.Add(Label);
			}
			*OutError = FString::Printf(
				TEXT("Multiple simulation stages exist. Pass one of: usage_id, stage_name, or stage_index. Available: [%s]"),
				*FString::Join(StageLabels, TEXT(", ")));
		}
		return false;
	}

	if (!TargetStage || !TargetStage->Script)
	{
		if (OutError) *OutError = TEXT("Simulation stage has no script");
		return false;
	}

	OutUsageId = TargetStage->Script->GetUsageId();
	if (OutStageName) *OutStageName = TargetStage->SimulationStageName.ToString();
	return true;
}

bool FMonolithNiagaraActions::ResolveEventHandlerSelector(UNiagaraSystem* System, const FString& EmitterHandleId,
	const TSharedPtr<FJsonObject>& Params, FGuid& OutUsageId, FString* OutEventName, FString* OutError)
{
	OutUsageId.Invalidate();
	if (OutEventName) OutEventName->Reset();
	if (OutError) OutError->Reset();

	if (!System)
	{
		if (OutError) *OutError = TEXT("System is null");
		return false;
	}

	const int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx == INDEX_NONE)
	{
		if (OutError) *OutError = FString::Printf(TEXT("Emitter '%s' not found"), *EmitterHandleId);
		return false;
	}

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EmitterIdx].GetEmitterData();
	if (!ED)
	{
		if (OutError) *OutError = TEXT("No emitter data");
		return false;
	}

	const TArray<FNiagaraEventScriptProperties>& Handlers = ED->GetEventHandlers();
	if (Handlers.Num() == 0)
	{
		if (OutError) *OutError = TEXT("Emitter has no event handlers");
		return false;
	}

	const FString UsageIdStr = Params->HasField(TEXT("usage_id")) ? Params->GetStringField(TEXT("usage_id")) : FString();
	const int32 HandlerIndex = Params->HasField(TEXT("handler_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("handler_index"))) : INDEX_NONE;

	const FNiagaraEventScriptProperties* TargetHandler = nullptr;
	if (!UsageIdStr.IsEmpty())
	{
		FGuid UsageId;
		if (!FGuid::Parse(UsageIdStr, UsageId))
		{
			if (OutError) *OutError = FString::Printf(TEXT("Invalid usage_id GUID '%s'"), *UsageIdStr);
			return false;
		}

		for (const FNiagaraEventScriptProperties& Handler : Handlers)
		{
			if (Handler.Script && Handler.Script->GetUsageId() == UsageId)
			{
				TargetHandler = &Handler;
				break;
			}
		}

		if (!TargetHandler)
		{
			if (OutError) *OutError = FString::Printf(TEXT("Event handler usage_id '%s' not found"), *UsageIdStr);
			return false;
		}
	}
	else if (HandlerIndex != INDEX_NONE)
	{
		if (!Handlers.IsValidIndex(HandlerIndex))
		{
			if (OutError) *OutError = FString::Printf(TEXT("Handler index %d out of range (0-%d)"), HandlerIndex, Handlers.Num() - 1);
			return false;
		}
		TargetHandler = &Handlers[HandlerIndex];
	}
	else if (Handlers.Num() == 1)
	{
		TargetHandler = &Handlers[0];
	}
	else
	{
		if (OutError)
		{
			TArray<FString> HandlerLabels;
			for (int32 Index = 0; Index < Handlers.Num(); ++Index)
			{
				const FNiagaraEventScriptProperties& Handler = Handlers[Index];
				FString Label = FString::Printf(TEXT("%d:%s"), Index, *Handler.SourceEventName.ToString());
				if (Handler.Script)
				{
					Label += FString::Printf(TEXT(" (%s)"), *Handler.Script->GetUsageId().ToString());
				}
				HandlerLabels.Add(Label);
			}
			*OutError = FString::Printf(
				TEXT("Multiple event handlers exist. Pass one of: usage_id or handler_index. Available: [%s]"),
				*FString::Join(HandlerLabels, TEXT(", ")));
		}
		return false;
	}

	if (!TargetHandler || !TargetHandler->Script)
	{
		if (OutError) *OutError = TEXT("Event handler has no script");
		return false;
	}

	OutUsageId = TargetHandler->Script->GetUsageId();
	if (OutEventName) *OutEventName = TargetHandler->SourceEventName.ToString();
	return true;
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
	case ENiagaraScriptUsage::ParticleEventScript: return TEXT("particle_event");
	case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("particle_simulation_stage");
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

UNiagaraNodeOutput* FMonolithNiagaraActions::FindOutputNode(UNiagaraSystem* System, const FString& EmitterHandleId,
	ENiagaraScriptUsage Usage, const FGuid& UsageId)
{
	UNiagaraGraph* Graph = GetGraphForUsage(System, EmitterHandleId, Usage);
	if (!Graph) return nullptr;
	return Graph->FindEquivalentOutputNode(Usage, UsageId);
}

UNiagaraNodeFunctionCall* FMonolithNiagaraActions::FindModuleNode(UNiagaraSystem* System, const FString& EmitterHandleId,
	const FString& NodeGuidStr, ENiagaraScriptUsage* OutUsage, FGuid* OutUsageId)
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
				if (OutUsageId) OutUsageId->Invalidate();
				return N;
			}
		}
	}

	// Pass 1b: Search selector-based shared-graph outputs before the generic graph fallback,
	// so event/simulation-stage modules keep their correct usage and usage_id.
	for (int32 EmitterIdx = 0; EmitterIdx < System->GetEmitterHandles().Num(); ++EmitterIdx)
	{
		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EmitterIdx];
		if (!EmitterHandleId.IsEmpty())
		{
			const FString HandleName = Handle.GetName().ToString();
			const FString HandleGuid = Handle.GetId().ToString();
			if (!HandleName.Equals(EmitterHandleId, ESearchCase::IgnoreCase)
				&& !HandleGuid.Equals(EmitterHandleId, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;

		for (UNiagaraSimulationStageBase* Stage : ED->GetSimulationStages())
		{
			if (!Stage || !Stage->Script) continue;
			const FGuid StageUsageId = Stage->Script->GetUsageId();
			UNiagaraNodeOutput* Out = FindOutputNode(System, Handle.GetId().ToString(),
				ENiagaraScriptUsage::ParticleSimulationStageScript, StageUsageId);
			if (!Out) continue;

			TArray<UNiagaraNodeFunctionCall*> Mods;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
			for (UNiagaraNodeFunctionCall* N : Mods)
			{
				if (!N) continue;
				if ((bHasGuid && N->NodeGuid == TargetGuid) || N->GetFunctionName() == NodeGuidStr)
				{
					if (OutUsage) *OutUsage = ENiagaraScriptUsage::ParticleSimulationStageScript;
					if (OutUsageId) *OutUsageId = StageUsageId;
					return N;
				}
			}
		}

		for (const FNiagaraEventScriptProperties& Handler : ED->GetEventHandlers())
		{
			if (!Handler.Script) continue;
			const FGuid EventUsageId = Handler.Script->GetUsageId();
			UNiagaraNodeOutput* Out = FindOutputNode(System, Handle.GetId().ToString(),
				ENiagaraScriptUsage::ParticleEventScript, EventUsageId);
			if (!Out) continue;

			TArray<UNiagaraNodeFunctionCall*> Mods;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*Out, Mods);
			for (UNiagaraNodeFunctionCall* N : Mods)
			{
				if (!N) continue;
				if ((bHasGuid && N->NodeGuid == TargetGuid) || N->GetFunctionName() == NodeGuidStr)
				{
					if (OutUsage) *OutUsage = ENiagaraScriptUsage::ParticleEventScript;
					if (OutUsageId) *OutUsageId = EventUsageId;
					return N;
				}
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
				if (OutUsageId) OutUsageId->Invalidate();
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
					if (OutUsageId) OutUsageId->Invalidate();
					return FN;
				}
				if (!bHasGuid && FN->GetFunctionName() == NodeGuidStr)
				{
					if (OutUsage) *OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
					if (OutUsageId) OutUsageId->Invalidate();
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
	if (L == TEXT("decal") || L == TEXT("decalrenderer")) return UNiagaraDecalRendererProperties::StaticClass();
	if (L == TEXT("volume") || L == TEXT("volumerenderer")) return UNiagaraVolumeRendererProperties::StaticClass();

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

FNiagaraTypeDefinition FMonolithNiagaraActions::ResolveNiagaraType(const FString& TypeName, bool* bOutFellBack)
{
	if (bOutFellBack) *bOutFellBack = false;
	FString L = TypeName.ToLower();
	// Bug fix: agents use "NiagaraFloat", "NiagaraBool", "Vector3f" etc. — handle the Niagara-prefixed
	// and HLSL-suffixed forms so they map correctly instead of silently falling back to float.
	if (L == TEXT("float") || L == TEXT("niagarafloat")) return FNiagaraTypeDefinition::GetFloatDef();
	if (L == TEXT("int") || L == TEXT("int32") || L == TEXT("integer") || L == TEXT("niagaraint") || L == TEXT("niagaraint32")) return FNiagaraTypeDefinition::GetIntDef();
	if (L == TEXT("bool") || L == TEXT("boolean") || L == TEXT("niagarabool")) return FNiagaraTypeDefinition::GetBoolDef();
	if (L == TEXT("vec2") || L == TEXT("vector2d") || L == TEXT("vector2") || L == TEXT("vector2f") || L == TEXT("fvector2f") || L == TEXT("float2")) return FNiagaraTypeDefinition::GetVec2Def();
	if (L == TEXT("vec3") || L == TEXT("vector") || L == TEXT("vector3") || L == TEXT("vector3f") || L == TEXT("fvector3f") || L == TEXT("niagaravector3f") || L == TEXT("float3")) return FNiagaraTypeDefinition::GetVec3Def();
	if (L == TEXT("vec4") || L == TEXT("vector4") || L == TEXT("vector4f") || L == TEXT("fvector4f") || L == TEXT("float4")) return FNiagaraTypeDefinition::GetVec4Def();
	if (L == TEXT("color") || L == TEXT("linearcolor") || L == TEXT("flinearcolor")) return FNiagaraTypeDefinition::GetColorDef();
	if (L == TEXT("position")) return FNiagaraTypeDefinition::GetPositionDef();
	if (L == TEXT("quat") || L == TEXT("quaternion")) return FNiagaraTypeDefinition::GetQuatDef();
	if (L == TEXT("matrix") || L == TEXT("matrix4")) return FNiagaraTypeDefinition::GetMatrix4Def();

	FString DICandidate = TypeName;
	DICandidate.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
	DICandidate.ReplaceInline(TEXT("-"), TEXT(""), ESearchCase::CaseSensitive);
	if (UClass* DIClass = MonolithNiagaraHelpers::ResolveNiagaraDataInterfaceClass(DICandidate))
	{
		return FNiagaraTypeDefinition(DIClass);
	}

	UE_LOG(LogMonolithNiagara, Warning, TEXT("ResolveNiagaraType: Unknown type '%s', defaulting to float"), *TypeName);
	if (bOutFellBack) *bOutFellBack = true;
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
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter_asset"), TEXT("string"), TEXT("Emitter asset path to add"))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Custom name for the emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_emitter"), TEXT("Remove an emitter from a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEmitter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("duplicate_emitter"), TEXT("Duplicate an emitter within a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateEmitter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("source_emitter"), TEXT("string"), TEXT("Name of emitter to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Name for the duplicated emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_enabled"), TEXT("Enable or disable an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterEnabled),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Whether to enable the emitter"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("reorder_emitters"), TEXT("Reorder emitters in a system"),
		FMonolithActionHandler::CreateStatic(&HandleReorderEmitters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("order"), TEXT("array"), TEXT("Array of emitter names in desired order"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_emitter_property"), TEXT("Set an emitter property"),
		FMonolithActionHandler::CreateStatic(&HandleSetEmitterProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("request_compile"), TEXT("Request compilation of a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleRequestCompile),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_system"), TEXT("Create a new Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSystem),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Path to save the new system"))
			.Optional(TEXT("template"), TEXT("string"), TEXT("Template system to base on"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_stateless_emitter"), TEXT("**Phase 0 stub.** Create a standalone UNiagaraStatelessEmitter (Lightweight Emitter) asset. Not yet implemented."),
		FMonolithActionHandler::CreateStatic(&HandleCreateStatelessEmitter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Path where the new Lightweight Emitter asset will be saved"))
			.Build());

	// Module (12)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_ordered_modules"), TEXT("Get ordered modules in a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleGetOrderedModules),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Script usage filter (e.g. particle_update, particle_event, emitter_spawn, particle_simulation_stage)"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name when usage is particle_simulation_stage"))
			.Optional(TEXT("usage_id"), TEXT("string"), TEXT("Simulation stage or event-handler usage ID when usage is particle_simulation_stage or particle_event"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("Simulation stage index when usage is particle_simulation_stage"))
			.Optional(TEXT("handler_index"), TEXT("integer"), TEXT("Event handler index when usage is particle_event"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_inputs"), TEXT("Get inputs for a module node"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleInputs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_graph"), TEXT("Get the node graph of a module script"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleGraph),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Module script asset path"))
			.Optional(TEXT("links"), TEXT("boolean"), TEXT("Also emit a top-level 'edges' array of pin-to-pin connections, plus 'pin_id' on every pin (default: false)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_custom_hlsl_text"), TEXT("Read the Custom HLSL source text from a Niagara script's CustomHlsl node"),
		FMonolithActionHandler::CreateStatic(&HandleGetCustomHLSLText),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Niagara script asset path"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Specific CustomHlsl node GUID when the script contains multiple nodes"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_custom_hlsl_text"), TEXT("Overwrite the Custom HLSL source text on a Niagara script's CustomHlsl node"),
		FMonolithActionHandler::CreateStatic(&HandleSetCustomHLSLText),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Niagara script asset path"))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("Replacement HLSL body text"))
			.Optional(TEXT("node_guid"), TEXT("string"), TEXT("Specific CustomHlsl node GUID when the script contains multiple nodes"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_module"), TEXT("Add a module to a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleAddModule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("usage"), TEXT("string"), TEXT("Script usage (particle_spawn, particle_update, particle_event, emitter_update, particle_simulation_stage)"))
			.Required(TEXT("module_script"), TEXT("string"), TEXT("Module script asset path"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name when usage is particle_simulation_stage"))
			.Optional(TEXT("usage_id"), TEXT("string"), TEXT("Simulation stage or event-handler usage ID when usage is particle_simulation_stage or particle_event"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("Simulation stage index when usage is particle_simulation_stage"))
			.Optional(TEXT("handler_index"), TEXT("integer"), TEXT("Event handler index when usage is particle_event"))
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Position to insert the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_module"), TEXT("Remove a module from a script stage"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveModule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("move_module"), TEXT("Move a module to a new index"),
		FMonolithActionHandler::CreateStatic(&HandleMoveModule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("new_index"), TEXT("integer"), TEXT("New position index"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_enabled"), TEXT("Enable or disable a module"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleEnabled),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("enabled"), TEXT("bool"), TEXT("Whether to enable the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_value"), TEXT("Set a module input value"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_binding"), TEXT("Bind a module input to a parameter"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputBinding),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("binding"), TEXT("string"), TEXT("Parameter binding path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_module_input_di"), TEXT("Set a data interface on a module input"),
		FMonolithActionHandler::CreateStatic(&HandleSetModuleInputDI),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
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
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path to save (e.g. /Game/VFX/Modules/MyModule)"))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("HLSL code body"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type} objects for input parameters"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for output parameters"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description for the module"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_function_from_hlsl"), TEXT("Create a Niagara function script from custom HLSL"),
		FMonolithActionHandler::CreateStatic(&HandleCreateFunctionFromHLSL),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Display name for the function"))
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset path to save (e.g. /Game/VFX/Functions/MyFunc)"))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("HLSL code body"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type} objects for input parameters"))
			.Optional(TEXT("outputs"), TEXT("array"), TEXT("Array of {name, type} objects for output parameters"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Optional description for the function"))
			.Build());

	// Parameter (9)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_all_parameters"), TEXT("Get all parameters in a system"),
		FMonolithActionHandler::CreateStatic(&HandleGetAllParameters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Filter to a specific emitter by name"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Filter by scope (e.g. 'User', 'ParticleSpawn', emitter name)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_user_parameters"), TEXT("Get user-exposed parameters"),
		FMonolithActionHandler::CreateStatic(&HandleGetUserParameters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_parameter_value"), TEXT("Get a parameter value"),
		FMonolithActionHandler::CreateStatic(&HandleGetParameterValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
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
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("parameter"), TEXT("string"), TEXT("Parameter name to trace"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_user_parameter"), TEXT("Add a user parameter"),
		FMonolithActionHandler::CreateStatic(&HandleAddUserParameter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("type"), TEXT("string"), TEXT("Niagara type name"))
			.Optional(TEXT("default"), TEXT("string"), TEXT("Default value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_user_parameter"), TEXT("Remove a user parameter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveUserParameter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_parameter_default"), TEXT("Set a parameter default value"),
		FMonolithActionHandler::CreateStatic(&HandleSetParameterDefault),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("parameter"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Default value to set"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_curve_value"), TEXT("Set curve keys on a module input"),
		FMonolithActionHandler::CreateStatic(&HandleSetCurveValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input parameter name"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("Array of curve key objects"))
			.Build());

	// Renderer (6)
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_renderer"), TEXT("Add a renderer to an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleAddRenderer),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("class"), TEXT("string"), TEXT("Renderer class (e.g. Sprite, Mesh, Ribbon)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_renderer"), TEXT("Remove a renderer from an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveRenderer),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Index of the renderer to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_material"), TEXT("Set renderer material"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererMaterial),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("material"), TEXT("string"), TEXT("Material asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_property"), TEXT("Set a renderer property"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_renderer_bindings"), TEXT("Get renderer attribute bindings"),
		FMonolithActionHandler::CreateStatic(&HandleGetRendererBindings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_binding"), TEXT("Set a renderer attribute binding"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererBinding),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("binding_name"), TEXT("string"), TEXT("Binding property name"))
			.Required(TEXT("attribute"), TEXT("string"), TEXT("Particle attribute to bind"))
			.Build());

	// Read (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_emitters"), TEXT("List all emitters in a Niagara system"),
		FMonolithActionHandler::CreateStatic(&HandleListEmitters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_renderers"), TEXT("List all renderers on a specific emitter"),
		FMonolithActionHandler::CreateStatic(&HandleListRenderers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());

	// Discovery (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_module_scripts"), TEXT("Search available Niagara module scripts by keyword"),
		FMonolithActionHandler::CreateStatic(&HandleListModuleScripts),
		FParamSchemaBuilder()
			.Optional(TEXT("search"), TEXT("string"), TEXT("Search keyword (e.g. 'gravity', 'color', 'velocity'). Omit to list all."))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Filter by usage: 'module', 'dynamic_input', 'function'. Default: module."))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results (default: 50)"))
			.Optional(TEXT("include_metadata"), TEXT("bool"), TEXT("Load each script to extract ModuleUsageBitmask, Category, Description (default: false — slower)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_renderer_properties"), TEXT("List editable properties on a renderer"),
		FMonolithActionHandler::CreateStatic(&HandleListRendererProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index (from list_renderers)"))
			.Build());

	// Batch (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("batch_execute"), TEXT("Execute multiple operations in one transaction"),
		FMonolithActionHandler::CreateStatic(&HandleBatchExecute),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("operations"), TEXT("array"), TEXT("Array of operation objects to execute"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_system_from_spec"), TEXT("Create a full system from JSON spec"),
		FMonolithActionHandler::CreateStatic(&HandleCreateSystemFromSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Asset save path, e.g. /Game/VFX/NS_MySystem"))
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
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());

	// Diagnostics (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_diagnostics"), TEXT("Get compile errors, warnings, renderer issues, and script stats"),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemDiagnostics),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("compile_first"), TEXT("boolean"), TEXT("Force synchronous compile before collecting diagnostics (default: true)"))
			.Build());

	// System Property (2)
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_property"), TEXT("Read a system-level property (WarmupTime, bDeterminism, RandomSeed, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name or snake_case alias: warmup_time, determinism, random_seed, max_pool_size, etc."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_system_property"), TEXT("Set a system-level property (WarmupTime, bDeterminism, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetSystemProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property: WarmupTime, WarmupTickCount, WarmupTickDelta, bFixedTickDelta, FixedTickDeltaTime, bDeterminism, RandomSeed, bSupportLargeWorldCoordinates, bNeedsSortedSignificanceHandling, SignificanceHandlerLink, MaxPoolSize"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Property value"))
			.Build());

	// Static Switch (1)
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_static_switch_value"), TEXT("Set a static switch value on a module"),
		FMonolithActionHandler::CreateStatic(&HandleSetStaticSwitchValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Static switch input name"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value to set (true/false for bool, enum value name for enums, integer for int switches)"))
			.Build());

	// --- Wave 2: Summary & Discovery (4 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_system_summary"), TEXT("One-call overview of an entire Niagara system, including emitter topology, event flow, location semantics, role hints, params, renderers, and module counts. AI guidance: start with compact for orientation; if emitters may be linked by events, switch to full before reasoning about where particles spawn or explode."),
		FMonolithActionHandler::CreateStatic(&HandleGetSystemSummary),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("Response verbosity: compact (default) or full. Compact returns topology and role hints without deep per-edge semantic payloads. If emitter-to-emitter event links may matter, prefer full."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_summary"), TEXT("Deep view of a single emitter, including modules per stage, renderers, event flow, spawn-location semantics, and role hints. AI guidance: use full when this emitter may send or receive Niagara events from other emitters."),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterSummary),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or GUID"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("Response verbosity: compact (default) or full. Compact keeps semantic fields shallow; full includes event/link and location-module details. If this emitter may participate in an event chain, prefer full."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_emitter_properties"), TEXT("List all editable properties on FVersionedNiagaraEmitterData with current values"),
		FMonolithActionHandler::CreateStatic(&HandleListEmitterProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_input_value"), TEXT("Read the current override value for a specific module input"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleInputValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name (bare or Module. prefixed)"))
			.Build());

	// --- Wave 3: DI Curve & Configuration (2 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_curve_keys"), TEXT("Set keys on a DataInterface curve input (NiagaraDataInterfaceCurve/ColorCurve). For plain float inputs use set_curve_value instead"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureCurveKeys),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name that has a curve DI"))
			.Required(TEXT("keys"), TEXT("array"), TEXT("For float: [{time,value}]; color: [{time,r,g,b,a}]; vector: [{time,x,y,z}]"))
			.Optional(TEXT("interp"), TEXT("string"), TEXT("Interpolation: linear, cubic, constant (default: cubic)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_data_interface"), TEXT("Set arbitrary properties on a DI attached to a module input via reflection"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureDataInterface),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Property name-value pairs to set on the DI"))
			.Build());

	// --- Wave 4: System Management (5 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("duplicate_system"), TEXT("Clone an entire Niagara system to a new path"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateSystem),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Source system asset path"))
			.RequiredAssetPath(TEXT("save_path"), TEXT("Destination path (e.g. /Game/VFX/NS_Fire_Copy)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_fixed_bounds"), TEXT("Set explicit bounds on system or emitter for GPU performance"),
		FMonolithActionHandler::CreateStatic(&HandleSetFixedBounds),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Emitter name (omit for system-level bounds)"))
			.Required(TEXT("min"), TEXT("array"), TEXT("Min bounds [x, y, z]"))
			.Required(TEXT("max"), TEXT("array"), TEXT("Max bounds [x, y, z]"))
			.Optional(TEXT("enabled"), TEXT("bool"), TEXT("Enable fixed bounds (default: true). Set false to re-enable dynamic."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_effect_type"), TEXT("Assign a UNiagaraEffectType for scalability and cull distance"),
		FMonolithActionHandler::CreateStatic(&HandleSetEffectType),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("effect_type"), TEXT("string"), TEXT("Effect type asset path, or 'none' to clear"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_emitter"), TEXT("Add a minimal empty emitter to a system (no template needed)"),
		FMonolithActionHandler::CreateStatic(&HandleCreateEmitter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Emitter name"))
			.Optional(TEXT("sim_target"), TEXT("string"), TEXT("cpu or gpu (default: cpu)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("export_system_spec"), TEXT("Reverse-engineer an existing system into create_system_from_spec-compatible JSON"),
		FMonolithActionHandler::CreateStatic(&HandleExportSystemSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("include_values"), TEXT("bool"), TEXT("Include current input override values (default: true)"))
			.Build());

	// --- Wave 5: Dynamic Inputs (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_dynamic_input"), TEXT("Attach a dynamic input script to a module input pin"),
		FMonolithActionHandler::CreateStatic(&HandleAddDynamicInput),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID or name"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Target module input name"))
			.Required(TEXT("dynamic_input_script"), TEXT("string"), TEXT("Asset path to the dynamic input script"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_dynamic_input_value"), TEXT("Set an input value on a dynamic input node"),
		FMonolithActionHandler::CreateStatic(&HandleSetDynamicInputValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
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
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_event_handler"), TEXT("Add an inter-emitter event handler (death, collision, location events). This only creates the handler and its ParticleEventScript container. It does NOT auto-add ReceiveDeathEvent/ReceiveLocationEvent modules. source_emitter is required for inter-emitter event handlers; unresolved handlers are rejected instead of being created with an empty SourceEmitterID. If the handler must consume source payloads such as Position/Velocity/Color, add the matching Receive<Event> module to the particle_event script and set the needed payload fields to Apply. For death-triggered bursts like fireworks, Position usually must be Apply."),
		FMonolithActionHandler::CreateStatic(&HandleAddEventHandler),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Target emitter that receives the event"))
			.Required(TEXT("event_name"), TEXT("string"), TEXT("Event name (CollisionEvent, DeathEvent, LocationEvent)"))
			.Optional(TEXT("source_emitter"), TEXT("string"), TEXT("Source emitter. Required for inter-emitter event handlers; omit only for intentional self-events."))
			.Optional(TEXT("execution_mode"), TEXT("string"), TEXT("every_particle, spawned_particles, single_particle (default: every_particle)"))
			.Optional(TEXT("max_events_per_frame"), TEXT("integer"), TEXT("Max events per frame (default: 0 = unlimited)"))
			.Optional(TEXT("spawn_number"), TEXT("integer"), TEXT("Spawn number for spawned_particles mode (default: 0)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("validate_system"), TEXT("Pre-compile validation: check common misconfigurations plus inter-emitter event-chain, spawn-location, and persistent-id issues"),
		FMonolithActionHandler::CreateStatic(&HandleValidateSystem),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_simulation_stage"), TEXT("Add a simulation stage to an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleAddSimulationStage),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Stage name"))
			.Optional(TEXT("iteration_source"), TEXT("string"), TEXT("particles or data_interface (default: particles)"))
			.Optional(TEXT("num_iterations"), TEXT("integer"), TEXT("Number of iterations (default: 1)"))
			.Build());

	// --- Composite Helpers (1 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_spawn_shape"),
		TEXT("Add a spawn shape (Cylinder, Sphere, Box, Cone, Torus) to an emitter with automatic switch setup"),
		FMonolithActionHandler::CreateStatic(&HandleSetSpawnShape),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("shape"), TEXT("string"), TEXT("Shape type: Cylinder, Sphere, Box, Cone, Torus"))
			.Optional(TEXT("params"), TEXT("object"), TEXT("Shape parameters (radius, height, surface_only, etc.)"))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("Remove existing location module if present (default: true)"))
			.Build());

	// --- Phase 3: Dynamic Input Features (5 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_dynamic_inputs"), TEXT("List dynamic inputs attached to a module's inputs"),
		FMonolithActionHandler::CreateStatic(&HandleListDynamicInputs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_dynamic_input_tree"), TEXT("Get recursive tree of all inputs on a module showing dynamic input structure"),
		FMonolithActionHandler::CreateStatic(&HandleGetDynamicInputTree),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max recursion depth (default: 10)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_dynamic_input"), TEXT("Remove a dynamic input from a module pin, cleaning up sub-nodes"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveDynamicInput),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Optional(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID (required with input)"))
			.Optional(TEXT("input"), TEXT("string"), TEXT("Input name on the module (required with module_node)"))
			.Optional(TEXT("dynamic_input_node"), TEXT("string"), TEXT("GUID of the dynamic input node (alternative to module_node+input)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_dynamic_input_value"), TEXT("Read a value from a dynamic input sub-pin"),
		FMonolithActionHandler::CreateStatic(&HandleGetDynamicInputValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("dynamic_input_node"), TEXT("string"), TEXT("GUID of the dynamic input node"))
			.Required(TEXT("input"), TEXT("string"), TEXT("Input name on the dynamic input"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_dynamic_input_inputs"), TEXT("Discover inputs on an unattached dynamic input script"),
		FMonolithActionHandler::CreateStatic(&HandleGetDynamicInputInputs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Asset path to the dynamic input script"))
			.Build());

	// --- Phase 4: Module & Emitter Management (2 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("rename_emitter"), TEXT("Rename an emitter in a system"),
		FMonolithActionHandler::CreateStatic(&HandleRenameEmitter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Current emitter name or handle ID"))
			.Required(TEXT("name"), TEXT("string"), TEXT("New emitter name"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_property"), TEXT("Read a single emitter property by name"),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name or snake_case alias (sim_target, local_space, determinism, bounds_mode, random_seed, allocation_mode, pre_allocation_count, requires_persistent_ids, max_gpu_particles_spawn_per_frame)"))
			.Build());

	// --- Phase 5: Renderer & DI Improvements (4 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_available_renderers"), TEXT("List all available Niagara renderer types with descriptions"),
		FMonolithActionHandler::CreateStatic(&HandleListAvailableRenderers),
		FParamSchemaBuilder()
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_renderer_mesh"), TEXT("Assign a StaticMesh to a MeshRenderer slot with optional scale/rotation/pivot"),
		FMonolithActionHandler::CreateStatic(&HandleSetRendererMesh),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Renderer index"))
			.Required(TEXT("mesh"), TEXT("string"), TEXT("StaticMesh asset path"))
			.Optional(TEXT("mesh_index"), TEXT("integer"), TEXT("Mesh slot index (default: 0)"))
			.Optional(TEXT("scale"), TEXT("object"), TEXT("Scale {x,y,z} (default: 1,1,1)"))
			.Optional(TEXT("rotation"), TEXT("object"), TEXT("Rotation {pitch,yaw,roll} in degrees"))
			.Optional(TEXT("pivot_offset"), TEXT("object"), TEXT("Pivot offset {x,y,z}"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_ribbon"), TEXT("High-level ribbon/trail/beam setup with presets (trail, beam, lightning, tube)"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureRibbon),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Ribbon renderer index"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Preset: trail, beam, lightning, tube"))
			.Optional(TEXT("facing_mode"), TEXT("string"), TEXT("Screen, Custom, CustomSideVector"))
			.Optional(TEXT("shape"), TEXT("string"), TEXT("Plane, MultiPlane, Tube, Custom"))
			.Optional(TEXT("tessellation_mode"), TEXT("string"), TEXT("Automatic, Custom, Disabled"))
			.Optional(TEXT("tessellation_factor"), TEXT("integer"), TEXT("Max tessellation factor (1-16)"))
			.Optional(TEXT("tube_subdivisions"), TEXT("integer"), TEXT("Tube subdivisions (3-16)"))
			.Optional(TEXT("uv_mode"), TEXT("string"), TEXT("ScaledUniformly, ScaledUsingRibbonSegmentLength, TiledOverRibbonLength, TiledFromStartOverRibbonLength"))
			.Optional(TEXT("tiling_length"), TEXT("number"), TEXT("UV tiling length in world units"))
			.Optional(TEXT("width_binding"), TEXT("string"), TEXT("Particle attribute for ribbon width"))
			.Optional(TEXT("link_order_binding"), TEXT("string"), TEXT("Particle attribute for ribbon link order"))
			.Optional(TEXT("ribbon_id_binding"), TEXT("string"), TEXT("Particle attribute for ribbon ID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("configure_subuv"), TEXT("Set up SubUV/flipbook animation on a sprite renderer"),
		FMonolithActionHandler::CreateStatic(&HandleConfigureSubUV),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name"))
			.Required(TEXT("renderer_index"), TEXT("integer"), TEXT("Sprite renderer index"))
			.Required(TEXT("columns"), TEXT("integer"), TEXT("Number of columns in the flipbook"))
			.Required(TEXT("rows"), TEXT("integer"), TEXT("Number of rows in the flipbook"))
			.Optional(TEXT("blend"), TEXT("bool"), TEXT("Enable sub-image blending (default: false)"))
			.Optional(TEXT("add_animation_module"), TEXT("bool"), TEXT("Add SubUVAnimation module to Particle Update (default: false)"))
			.Optional(TEXT("playback_mode"), TEXT("string"), TEXT("Playback mode for animation module"))
			.Optional(TEXT("start_frame"), TEXT("number"), TEXT("Start frame for animation"))
			.Optional(TEXT("end_frame"), TEXT("number"), TEXT("End frame for animation"))
			.Build());

	// --- Phase 6A: Event Handlers, Simulation Stages, Module Outputs (7 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_event_handlers"), TEXT("Read all event handlers on an emitter with full properties"),
		FMonolithActionHandler::CreateStatic(&HandleGetEventHandlers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_event_handler_property"), TEXT("Modify an event handler property (ExecutionMode, SpawnNumber, MaxEventsPerFrame, SourceEventName, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetEventHandlerProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property: ExecutionMode, SpawnNumber, MaxEventsPerFrame, SourceEventName, bRandomSpawnNumber, MinSpawnNumber, UpdateAttributeInitialValues"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to set"))
			.Optional(TEXT("handler_index"), TEXT("integer"), TEXT("Event handler index"))
			.Optional(TEXT("usage_id"), TEXT("string"), TEXT("Event handler script usage ID (alternative to handler_index)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_event_handler"), TEXT("Remove an event handler from an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveEventHandler),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Optional(TEXT("handler_index"), TEXT("integer"), TEXT("Event handler index"))
			.Optional(TEXT("usage_id"), TEXT("string"), TEXT("Event handler script usage ID (alternative to handler_index)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_simulation_stages"), TEXT("Read all simulation stages on an emitter with full properties"),
		FMonolithActionHandler::CreateStatic(&HandleGetSimulationStages),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_simulation_stage_property"), TEXT("Modify a simulation stage property (IterationSource, NumIterations, SimulationStageName, etc.)"),
		FMonolithActionHandler::CreateStatic(&HandleSetSimulationStageProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name (UProperty or snake_case alias)"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Value to set"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("Simulation stage index"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (alternative to stage_index)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_simulation_stage"), TEXT("Remove a simulation stage from an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveSimulationStage),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("Simulation stage index"))
			.Optional(TEXT("stage_name"), TEXT("string"), TEXT("Simulation stage name (alternative to stage_index)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_output_parameters"), TEXT("Discover what particle attributes a module writes to (output variables)"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleOutputParameters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID"))
			.Build());

	// --- Phase 6B: NPC Support (5 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_npc"), TEXT("Create a Niagara Parameter Collection (NPC) asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateNPC),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Path to save the NPC (e.g. /Game/VFX/NPC_Global)"))
			.Required(TEXT("namespace"), TEXT("string"), TEXT("FName namespace for the collection"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_npc"), TEXT("Read all parameters and defaults from a Niagara Parameter Collection"),
		FMonolithActionHandler::CreateStatic(&HandleGetNPC),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NPC asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("add_npc_parameter"), TEXT("Add a parameter to a Niagara Parameter Collection"),
		FMonolithActionHandler::CreateStatic(&HandleAddNPCParameter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NPC asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("type"), TEXT("string"), TEXT("Parameter type (float, int, bool, vec2, vec3, vec4, color, position)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("remove_npc_parameter"), TEXT("Remove a parameter from a Niagara Parameter Collection"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveNPCParameter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NPC asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name to remove"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_npc_default"), TEXT("Set the default value for a parameter in a Niagara Parameter Collection"),
		FMonolithActionHandler::CreateStatic(&HandleSetNPCDefault),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NPC asset path"))
			.Required(TEXT("name"), TEXT("string"), TEXT("Parameter name"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Default value (number, bool, or {x,y,z}/{r,g,b,a} object)"))
			.Build());

	// --- Phase 6B: Effect Type CRUD (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("create_effect_type"), TEXT("Create a UNiagaraEffectType asset with optional initial settings"),
		FMonolithActionHandler::CreateStatic(&HandleCreateEffectType),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("save_path"), TEXT("Path to save the effect type (e.g. /Game/VFX/ET_Ambient)"))
			.Optional(TEXT("cull_reaction"), TEXT("string"), TEXT("Deactivate, DeactivateImmediate, DeactivateResume, PauseResume (default: Deactivate)"))
			.Optional(TEXT("update_frequency"), TEXT("string"), TEXT("Continuous, Low, Medium, High (default: Continuous)"))
			.Optional(TEXT("max_distance"), TEXT("number"), TEXT("Significance max distance (default: 0 = no limit)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_effect_type"), TEXT("Read all settings from a UNiagaraEffectType asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetEffectType),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Effect type asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_effect_type_property"), TEXT("Set a property on a UNiagaraEffectType via reflection"),
		FMonolithActionHandler::CreateStatic(&HandleSetEffectTypeProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Effect type asset path"))
			.Required(TEXT("property"), TEXT("string"), TEXT("Property name (CullReaction, UpdateFrequency, SignificanceHandler, etc.)"))
			.Required(TEXT("value"), TEXT("any"), TEXT("Property value"))
			.Build());

	// --- Phase 6B: Parameter Discovery (1 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_available_parameters"), TEXT("List all parameters available for binding in a system (user, engine, particle, emitter, system attributes)"),
		FMonolithActionHandler::CreateStatic(&HandleGetAvailableParameters),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("emitter"), TEXT("string"), TEXT("Emitter name (to include particle/emitter-scoped attributes)"))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Filter by context: user, engine, particle, emitter, system, or all (default: all)"))
			.Build());

	// --- Phase 6B: Preview (1 new, QoL params added Phase 9) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("preview_system"), TEXT("Capture a preview screenshot of a Niagara system via editor preview scene"),
		FMonolithActionHandler::CreateStatic(&HandlePreviewSystem),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("seek_time"), TEXT("number"), TEXT("Simulation time to seek to before capture (default: 1.0)"))
			.Optional(TEXT("resolution"), TEXT("string"), TEXT("Resolution as WxH string (default: 512x512)"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Absolute output file path (default: auto-generated in Saved/Screenshots/Monolith)"))
			.Optional(TEXT("camera_angle"), TEXT("string"), TEXT("Camera preset: front, top, three_quarter, side (default: three_quarter)"))
			.Optional(TEXT("background_color"), TEXT("array"), TEXT("Background color [R,G,B,A] in 0-1 range (default: [0,0,0,1])"))
			.Optional(TEXT("auto_fit"), TEXT("bool"), TEXT("Auto-compute camera distance from system bounds (default: true)"))
			.Build());

	// --- Phase 7: Advanced Features (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("diff_systems"), TEXT("Compare two Niagara systems and return structured diff of emitters, modules, inputs, renderers, parameters, properties"),
		FMonolithActionHandler::CreateStatic(&HandleDiffSystems),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path_a"), TEXT("First Niagara system asset path"))
			.RequiredAssetPath(TEXT("asset_path_b"), TEXT("Second Niagara system asset path"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("summary or full (default: full)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("save_emitter_as_template"), TEXT("Extract a configured emitter from a system and save as standalone emitter asset"),
		FMonolithActionHandler::CreateStatic(&HandleSaveEmitterAsTemplate),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Source Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID to extract"))
			.RequiredAssetPath(TEXT("save_path"), TEXT("Target asset path for standalone emitter (e.g. /Game/VFX/Templates/E_MyTemplate)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("clone_module_overrides"), TEXT("Copy inline value overrides and bindings from one module to another (same script required)"),
		FMonolithActionHandler::CreateStatic(&HandleCloneModuleOverrides),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("source_emitter"), TEXT("string"), TEXT("Source emitter name or handle ID"))
			.Required(TEXT("source_module"), TEXT("string"), TEXT("Source module node GUID"))
			.Required(TEXT("target_emitter"), TEXT("string"), TEXT("Target emitter name or handle ID"))
			.Required(TEXT("target_module"), TEXT("string"), TEXT("Target module node GUID"))
			.Build());

	// --- Phase 8: Expansion (4 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("save_system"), TEXT("Save a Niagara asset (system, script, NPC, effect type) to disk"),
		FMonolithActionHandler::CreateStatic(&HandleSaveSystem),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara asset path to save"))
			.Optional(TEXT("only_if_dirty"), TEXT("bool"), TEXT("Only save if the asset has unsaved changes (default: true)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_static_switch_value"), TEXT("Get static switch value(s) on a module — omit input to list all switches"),
		FMonolithActionHandler::CreateStatic(&HandleGetStaticSwitchValue),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Required(TEXT("module_node"), TEXT("string"), TEXT("Module node GUID"))
			.Optional(TEXT("input"), TEXT("string"), TEXT("Static switch name — omit to list all"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("import_system_spec"), TEXT("Overwrite an existing Niagara system with a JSON spec (removes all emitters/params, applies spec fresh)"),
		FMonolithActionHandler::CreateStatic(&HandleImportSystemSpec),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Existing Niagara system to overwrite"))
			.Required(TEXT("spec"), TEXT("object"), TEXT("System spec JSON (same format as create_system_from_spec)"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Import mode: overwrite or merge"))
			.Build());

	// --- Phase 9: Medium Priority Expansion (6 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_di_properties"), TEXT("Inspect editable properties on a Niagara DataInterface class (CDO reflection)"),
		FMonolithActionHandler::CreateStatic(&HandleGetDIProperties),
		FParamSchemaBuilder()
			.Required(TEXT("di_class"), TEXT("string"), TEXT("Data interface class name (e.g. NiagaraDataInterfaceColorCurve, SkelMesh, StaticMesh)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("clear_emitter_modules"), TEXT("Remove all modules from an emitter, optionally filtered by stage"),
		FMonolithActionHandler::CreateStatic(&HandleClearEmitterModules),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Optional(TEXT("usage"), TEXT("string"), TEXT("Stage filter: particle_update, particle_spawn, emitter_update, emitter_spawn, or all (default: all)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_module_script_inputs"), TEXT("Introspect a module script's inputs WITHOUT adding it to an emitter"),
		FMonolithActionHandler::CreateStatic(&HandleGetModuleScriptInputs),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("script_path"), TEXT("Module script asset path (e.g. /Niagara/Modules/Update/Forces/Gravity.Gravity)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_scalability_settings"), TEXT("Read scalability settings from a NiagaraEffectType asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetScalabilitySettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NiagaraEffectType asset path"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("set_scalability_settings"), TEXT("Set scalability settings on a NiagaraEffectType asset"),
		FMonolithActionHandler::CreateStatic(&HandleSetScalabilitySettings),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("NiagaraEffectType asset path"))
			.Required(TEXT("settings"), TEXT("array"), TEXT("Array of scalability configs: [{quality_levels:[0,1,...], max_distance, max_instances, max_system_instances}]"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_systems"), TEXT("Search/list Niagara system assets in the project"),
		FMonolithActionHandler::CreateStatic(&HandleListSystems),
		FParamSchemaBuilder()
			.Optional(TEXT("search"), TEXT("string"), TEXT("Keyword filter (tokenized, case-insensitive)"))
			.OptionalAssetPath(TEXT("path"), TEXT("Content path filter (e.g. /Game/VFX)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results (default: 50)"))
			.Build());

	// --- Phase 10: Low Priority & QoL (3 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("duplicate_module"), TEXT("Duplicate a module within or across emitters (copies script + all overrides)"),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateModule),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("source_emitter"), TEXT("string"), TEXT("Source emitter name or handle ID"))
			.Required(TEXT("source_module_node"), TEXT("string"), TEXT("Source module node GUID to duplicate"))
			.Optional(TEXT("target_emitter"), TEXT("string"), TEXT("Target emitter (default: same as source)"))
			.Optional(TEXT("target_usage"), TEXT("string"), TEXT("Target stage (default: same as source module)"))
			.Optional(TEXT("target_stage_name"), TEXT("string"), TEXT("Simulation stage name when target_usage is particle_simulation_stage"))
			.Optional(TEXT("usage_id"), TEXT("string"), TEXT("Simulation stage usage ID when targeting a specific stage"))
			.Optional(TEXT("stage_index"), TEXT("integer"), TEXT("Simulation stage index when targeting a specific stage"))
			.Optional(TEXT("target_index"), TEXT("integer"), TEXT("Insertion index in target stage (default: append)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("get_emitter_parent"), TEXT("Get the parent emitter asset of an emitter in a system (read-only)"),
		FMonolithActionHandler::CreateStatic(&HandleGetEmitterParent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("emitter"), TEXT("string"), TEXT("Emitter name or handle ID"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("rename_user_parameter"), TEXT("Rename a user parameter and update all module bindings that reference it"),
		FMonolithActionHandler::CreateStatic(&HandleRenameUserParameter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Required(TEXT("old_name"), TEXT("string"), TEXT("Current parameter name (with or without User. prefix)"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New parameter name (with or without User. prefix)"))
			.Build());

	// --- Tranche 2 (#64): read-only Search & Discovery + per-system DI (7 new) ---
	Registry.RegisterAction(TEXT("niagara"), TEXT("search_by_parameter"), TEXT("Find Niagara systems exposing a user parameter whose name (case-insensitive) contains the query. Optional type filter."),
		FMonolithActionHandler::CreateStatic(&HandleSearchByParameter),
		FParamSchemaBuilder()
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name substring to match (case-insensitive, partial)"))
			.Optional(TEXT("parameter_type"), TEXT("string"), TEXT("Optional type filter (e.g. float, Vector, LinearColor) matched against the parameter's type name"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Content path to restrict the scan (e.g. /Game/VFX)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max matching systems (default: 50)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("search_by_data_interface"), TEXT("Find Niagara systems using a Data Interface whose class name (case-insensitive) contains the query. Per-system traversal via FNiagaraDataInterfaceUtilities::ForEachDataInterface."),
		FMonolithActionHandler::CreateStatic(&HandleSearchByDataInterface),
		FParamSchemaBuilder()
			.Required(TEXT("di_class"), TEXT("string"), TEXT("Data interface class-name substring (e.g. NiagaraDataInterfaceCurve, Curve, Grid2D)"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Content path to restrict the scan (e.g. /Game/VFX)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max matching systems (default: 50). NOTE: this action loads each system — limit + folder are the cost governors."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("query_niagara"), TEXT("Structured-filter query over all systems. Conditions joined by AND, comma- or AND-separated: emitters>N / emitters<N / emitters=N, sim_target=GPU|CPU, has_renderer=<name>. Deterministic DSL, NOT natural language."),
		FMonolithActionHandler::CreateStatic(&HandleQueryNiagara),
		FParamSchemaBuilder()
			.Required(TEXT("query_string"), TEXT("string"), TEXT("e.g. 'emitters=2, sim_target=GPU' or 'emitters>1 AND has_renderer=Mesh'"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Content path to restrict the scan (e.g. /Game/VFX)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max matching systems (default: 50)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("find_similar_systems"), TEXT("Rank systems by structural similarity to a reference system. Score = weighted blend of emitter-count proximity, renderer-class-set Jaccard, and module-name-set Jaccard. Reference scores 1.0 against itself."),
		FMonolithActionHandler::CreateStatic(&HandleFindSimilarSystems),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Reference Niagara system asset path"))
			.Optional(TEXT("threshold"), TEXT("number"), TEXT("Minimum similarity score to include (0..1, default: 0.5)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max ranked matches (default: 10)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("search_by_material"), TEXT("Find Niagara systems whose emitter renderers reference a given material. Walks each system's emitters' renderers' material bindings (Sprite/Ribbon/Mesh)."),
		FMonolithActionHandler::CreateStatic(&HandleSearchByMaterial),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("material_path"), TEXT("Material / MaterialInterface asset path to find users of"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Content path to restrict the scan (e.g. /Game/VFX)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max matching systems (default: 50). Loads each system — limit + folder are the cost governors."))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("find_niagara_references"), TEXT("Find all assets that reference a given Niagara asset, via the Asset Registry referencer graph (IAssetRegistry::GetReferencers)."),
		FMonolithActionHandler::CreateStatic(&HandleFindNiagaraReferences),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara asset path to find referencers of"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max referencers returned (default: 100)"))
			.Build());
	Registry.RegisterAction(TEXT("niagara"), TEXT("list_system_data_interfaces"), TEXT("Enumerate the Data Interfaces actually USED BY a given system (per-system traversal via FNiagaraDataInterfaceUtilities::ForEachDataInterface). Distinct from get_di_properties (CDO-class reflection only)."),
		FMonolithActionHandler::CreateStatic(&HandleListSystemDataInterfaces),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Build());
}

// ============================================================================
// System Actions (8)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	//
	// If the source emitter is an emitter subobject owned by another NiagaraSystem, adding it directly keeps
	// parent/merge chain references back to that foreign system's private emitter object. That can later crash
	// during save or editor refresh. For embedded emitters, first duplicate without merging so the parent chain is severed.
	UNiagaraEmitter* SafeEmitterAsset = EmitterAsset;
	TStrongObjectPtr<UNiagaraEmitter> DetachedEmbeddedEmitter;
	if (EmitterAsset->GetTypedOuter<UNiagaraSystem>() != nullptr)
	{
		DetachedEmbeddedEmitter.Reset(EmitterAsset->DuplicateWithoutMerging(GetTransientPackage()));
		if (!DetachedEmbeddedEmitter.IsValid())
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to create detached copy of embedded emitter '%s' before add_emitter"), *EmitterAssetPath));
		}

		SafeEmitterAsset = DetachedEmbeddedEmitter.Get();
	}
	const FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System, *SafeEmitterAsset, SafeEmitterAsset->GetExposedVersion().VersionGuid);

	GEditor->EndTransaction();

	// Validate the emitter was actually added (engine can silently fail)
	if (System->GetEmitterHandles().Num() <= HandleCountBefore || !NewHandleId.IsValid())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to add emitter from '%s'. The asset may not be a valid NiagaraEmitter or may be incompatible."),
			*EmitterAssetPath));
	}

	// Regenerate node GUIDs on the new emitter to avoid collisions when adding
	// the same emitter template multiple times (duplicate GUIDs cause FindModuleNode
	// to resolve to the wrong emitter's modules)
	int32 NewIdx = INDEX_NONE;
	for (int32 i = 0; i < System->GetEmitterHandles().Num(); ++i)
	{
		if (System->GetEmitterHandles()[i].GetId() == NewHandleId)
		{
			NewIdx = i;
			break;
		}
	}
	if (NewIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[NewIdx].GetEmitterData();
		UNiagaraScriptSource* Source = ED ? Cast<UNiagaraScriptSource>(ED->GraphSource) : nullptr;
		if (Source && Source->NodeGraph)
		{
			for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
			{
				Node->CreateNewGuid();
			}
			Source->NodeGraph->NotifyGraphChanged();
		}
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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(TEXT("Emitter removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleDuplicateEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEmitterEnabled(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(bEnabled ? TEXT("Emitter enabled") : TEXT("Emitter disabled"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleReorderEmitters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(TEXT("Emitters reordered"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEmitterProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return bOk ? NA_SuccessStr(TEXT("Property set")) : FMonolithActionResult::Error(TEXT("Unknown property"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleRequestCompile(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	bool bForce = Params->HasField(TEXT("force")) && Params->GetBoolField(TEXT("force"));
	bool bSync = Params->HasField(TEXT("synchronous")) && Params->GetBoolField(TEXT("synchronous"));

	System->RequestCompile(bForce);
	if (bSync)
	{
		System->WaitForCompilationComplete();
	}
	return NA_SuccessStr(TEXT("Compile requested"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	FString TemplatePath = Params->HasField(TEXT("template")) ? Params->GetStringField(TEXT("template")) : FString();

	// Validate the destination before loading the template or touching AssetTools —
	// a malformed path (e.g. "//Game/Foo") asserts inside CreatePackage.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	if (!TemplatePath.IsEmpty())
	{
		UNiagaraSystem* Template = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(TemplatePath);
		if (!Template) return FMonolithActionResult::Error(TEXT("Failed to load template"));

		const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);

		IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Dup = AT.DuplicateAsset(AssetName, PackagePath, Template);
		if (!Dup) return FMonolithActionResult::Error(TEXT("Failed to duplicate template"));
		return NA_SuccessStr(Dup->GetPathName());
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);

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

	return NA_SuccessStr(NS->GetPathName());
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateStatelessEmitter(const TSharedPtr<FJsonObject>& Params)
{
	// Phase 1 — create a standalone Lightweight (stateless) Emitter asset.
	//
	// IRON LAW 1 NOTE: the plan flagged `UNiagaraStatelessEmitterFactoryNew` as an
	// unverified convention guess. source_query confirms NO such factory exists in
	// UE 5.7 — only `UNiagaraStatelessEmitterTemplateFactoryNew` (a different class,
	// creates `UNiagaraStatelessEmitterTemplate`, gated by a console var). In the
	// engine, `UNiagaraStatelessEmitter` is always created via direct `NewObject` as
	// a sub-object of a `UNiagaraSystem` (NiagaraEmitterHandle.cpp:219,
	// NiagaraSystemViewModel.cpp:653/:672). We mirror `HandleCreateSystem`'s
	// package + NewObject pattern to author a standalone .uasset whose top-level
	// object is the stateless emitter itself.
	FString SavePath = Params->GetStringField(TEXT("save_path"));

	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);

	FString FullPath = PackagePath / AssetName;
	UPackage* Pkg = CreatePackage(*FullPath);
	if (!Pkg) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	// Resolve UNiagaraStatelessEmitter's UClass at runtime via FindObject — its
	// header lives under Niagara/Internal/ which is intentionally not exposed to
	// dependent modules. Standard UE pattern for cross-plugin private-type access.
	UClass* StatelessClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraStatelessEmitter"));
	if (!StatelessClass) return FMonolithActionResult::Error(TEXT("UNiagaraStatelessEmitter UClass not found - Niagara plugin may not be loaded"));

	UObject* Emitter = NewObject<UObject>(
		Pkg, StatelessClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!Emitter) return FMonolithActionResult::Error(TEXT("Failed to create stateless emitter"));

	FAssetRegistryModule::AssetCreated(Emitter);
	Pkg->MarkPackageDirty();

	// Save to disk immediately so the .uasset file exists. Mirrors HandleCreateSystem.
	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(Pkg, Emitter, *PackageFilename, SaveArgs);
	}

	return NA_SuccessStr(Emitter->GetPathName());
}

// ============================================================================
// Module Actions (12)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetOrderedModules(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ScriptUsage = Params->GetStringField(TEXT("usage"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Determine which usages to query
	TArray<ENiagaraScriptUsage> UsagesToQuery;
	TArray<TPair<FGuid, FString>> SimulationStagesToQuery;
	TArray<TPair<FGuid, FString>> EventHandlersToQuery;
	if (ScriptUsage.IsEmpty())
	{
		// No usage specified — return ALL stages
		UsagesToQuery = {
			ENiagaraScriptUsage::EmitterSpawnScript, ENiagaraScriptUsage::EmitterUpdateScript,
			ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript,
		};

		if (!EmitterHandleId.IsEmpty())
		{
			const int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
			if (EmitterIdx != INDEX_NONE)
			{
				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EmitterIdx].GetEmitterData();
				if (ED)
				{
					for (const FNiagaraEventScriptProperties& Handler : ED->GetEventHandlers())
					{
						if (!Handler.Script) continue;
						EventHandlersToQuery.Add(TPair<FGuid, FString>(Handler.Script->GetUsageId(), Handler.SourceEventName.ToString()));
					}

					for (UNiagaraSimulationStageBase* Stage : ED->GetSimulationStages())
					{
						if (!Stage || !Stage->Script) continue;
						SimulationStagesToQuery.Add(TPair<FGuid, FString>(Stage->Script->GetUsageId(), Stage->SimulationStageName.ToString()));
					}
				}
			}
		}
	}
	else
	{
		ENiagaraScriptUsage Usage;
		if (!ResolveScriptUsage(ScriptUsage, Usage))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unrecognized usage '%s'. Valid values: system_spawn, system_update, emitter_spawn, emitter_update, particle_spawn (or spawn), particle_update (or update), particle_event (or event), particle_simulation_stage (or simulation_stage)"),
				*ScriptUsage));
		}

		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			FGuid UsageId;
			FString StageName;
			FString StageError;
			if (!ResolveSimulationStageSelector(System, EmitterHandleId, Params, UsageId, &StageName, &StageError))
			{
				return FMonolithActionResult::Error(StageError);
			}
			SimulationStagesToQuery.Add(TPair<FGuid, FString>(UsageId, StageName));
		}
		else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			FGuid UsageId;
			FString EventName;
			FString EventError;
			if (!ResolveEventHandlerSelector(System, EmitterHandleId, Params, UsageId, &EventName, &EventError))
			{
				return FMonolithActionResult::Error(EventError);
			}
			EventHandlersToQuery.Add(TPair<FGuid, FString>(UsageId, EventName));
		}
		else
		{
			UsagesToQuery.Add(Usage);
		}
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

	for (const TPair<FGuid, FString>& StageEntry : SimulationStagesToQuery)
	{
		UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId,
			ENiagaraScriptUsage::ParticleSimulationStageScript, StageEntry.Key);
		if (!OutputNode) continue;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);
		for (int32 i = 0; i < ModuleNodes.Num(); ++i)
		{
			UNiagaraNodeFunctionCall* N = ModuleNodes[i];
			if (!N) continue;
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("node_guid"), N->NodeGuid.ToString());
			M->SetStringField(TEXT("function_name"), N->GetFunctionName());
			M->SetNumberField(TEXT("index"), i);
			M->SetStringField(TEXT("usage"), TEXT("particle_simulation_stage"));
			if (!StageEntry.Value.IsEmpty()) M->SetStringField(TEXT("stage_name"), StageEntry.Value);
			M->SetStringField(TEXT("usage_id"), StageEntry.Key.ToString());
			TOptional<bool> bEn = MonolithNiagaraHelpers::GetModuleIsEnabled(*N);
			M->SetBoolField(TEXT("enabled"), bEn.IsSet() ? bEn.GetValue() : true);
			if (N->FunctionScript) M->SetStringField(TEXT("script_path"), N->FunctionScript->GetPathName());
			Arr.Add(MakeShared<FJsonValueObject>(M));
		}
	}

	for (const TPair<FGuid, FString>& HandlerEntry : EventHandlersToQuery)
	{
		UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId,
			ENiagaraScriptUsage::ParticleEventScript, HandlerEntry.Key);
		if (!OutputNode) continue;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);
		for (int32 i = 0; i < ModuleNodes.Num(); ++i)
		{
			UNiagaraNodeFunctionCall* N = ModuleNodes[i];
			if (!N) continue;
			TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
			M->SetStringField(TEXT("node_guid"), N->NodeGuid.ToString());
			M->SetStringField(TEXT("function_name"), N->GetFunctionName());
			M->SetNumberField(TEXT("index"), i);
			M->SetStringField(TEXT("usage"), TEXT("particle_event"));
			if (!HandlerEntry.Value.IsEmpty()) M->SetStringField(TEXT("event_name"), HandlerEntry.Value);
			M->SetStringField(TEXT("usage_id"), HandlerEntry.Key.ToString());
			TOptional<bool> bEn = MonolithNiagaraHelpers::GetModuleIsEnabled(*N);
			M->SetBoolField(TEXT("enabled"), bEn.IsSet() ? bEn.GetValue() : true);
			if (N->FunctionScript) M->SetStringField(TEXT("script_path"), N->FunctionScript->GetPathName());
			Arr.Add(MakeShared<FJsonValueObject>(M));
		}
	}
	return NA_SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	// Opt-in link topology. Default false so the response stays byte-identical to the
	// pre-`links` shape for every existing caller.
	const bool bIncludeLinks = Params->HasField(TEXT("links")) && Params->GetBoolField(TEXT("links"));

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script) return FMonolithActionResult::Error(TEXT("Failed to load script"));

	UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Src || !Src->NodeGraph) return FMonolithActionResult::Error(TEXT("No graph available"));

	UNiagaraGraph* Graph = Src->NodeGraph;
	TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
	Res->SetStringField(TEXT("script_path"), ScriptPath);
	Res->SetStringField(TEXT("script_usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage())));

	// Edge accumulation for links=true. Every connection is reachable from BOTH of its
	// endpoints, so the pin walk below sees each edge twice; SeenEdges deduplicates on a
	// canonical (endpoint, endpoint) key so the array carries one entry per connection.
	TArray<TSharedPtr<FJsonValue>> EdgesArr;
	TSet<FString> SeenEdges;

	TArray<TSharedPtr<FJsonValue>> NodesArr;
	TArray<UEdGraphNode*> AllNodes;
	Graph->GetNodesOfClass<UEdGraphNode>(AllNodes);
	for (UEdGraphNode* Node : AllNodes)
	{
		// Named NodeObj instead of NO to avoid Apple <objc/objc.h> macro `#define NO __objc_no`
		// that leaks in transitively via ApplePlatformProcess.h on macOS and breaks compilation.
		TSharedRef<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
		NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
		NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
		if (UNiagaraNodeFunctionCall* FN = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			NodeObj->SetStringField(TEXT("function_name"), FN->GetFunctionName());
			if (FN->FunctionScript) NodeObj->SetStringField(TEXT("function_script"), FN->FunctionScript->GetPathName());
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
			if (bIncludeLinks)
			{
				// PinId is NOT unique on its own — copy-pasting a node duplicates its pin
				// GUIDs — so an edge endpoint is always the (node_guid, pin_id) PAIR.
				PO->SetStringField(TEXT("pin_id"), Pin->PinId.ToString());

				const FString ThisEnd = Node->NodeGuid.ToString() + TEXT(":") + Pin->PinId.ToString();
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked) continue;
					UEdGraphNode* LinkedNode = Linked->GetOwningNodeUnchecked();
					if (!LinkedNode) continue;

					const FString OtherEnd = LinkedNode->NodeGuid.ToString() + TEXT(":") + Linked->PinId.ToString();
					const bool bThisFirst = FCString::Strcmp(*ThisEnd, *OtherEnd) <= 0;
					const FString EdgeKey = bThisFirst
						? (ThisEnd + TEXT("|") + OtherEnd)
						: (OtherEnd + TEXT("|") + ThisEnd);
					if (SeenEdges.Contains(EdgeKey)) continue;
					SeenEdges.Add(EdgeKey);

					// Orient from -> to along data flow (output pin is the source). A
					// malformed graph with matching directions falls back to the canonical
					// key order so the orientation is still deterministic.
					bool bThisIsSource = (Pin->Direction == EGPD_Output);
					if (Pin->Direction == Linked->Direction) bThisIsSource = bThisFirst;

					UEdGraphNode* FromNode = bThisIsSource ? Node : LinkedNode;
					UEdGraphPin* FromPin = bThisIsSource ? Pin : Linked;
					UEdGraphNode* ToNode = bThisIsSource ? LinkedNode : Node;
					UEdGraphPin* ToPin = bThisIsSource ? Linked : Pin;

					TSharedRef<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
					EdgeObj->SetStringField(TEXT("from_node_guid"), FromNode->NodeGuid.ToString());
					EdgeObj->SetStringField(TEXT("from_pin_id"), FromPin->PinId.ToString());
					EdgeObj->SetStringField(TEXT("from_pin_name"), FromPin->PinName.ToString());
					EdgeObj->SetStringField(TEXT("to_node_guid"), ToNode->NodeGuid.ToString());
					EdgeObj->SetStringField(TEXT("to_pin_id"), ToPin->PinId.ToString());
					EdgeObj->SetStringField(TEXT("to_pin_name"), ToPin->PinName.ToString());
					EdgesArr.Add(MakeShared<FJsonValueObject>(EdgeObj));
				}
			}
			PinsArr.Add(MakeShared<FJsonValueObject>(PO));
		}
		NodeObj->SetArrayField(TEXT("pins"), PinsArr);
		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Res->SetArrayField(TEXT("nodes"), NodesArr);
	if (bIncludeLinks)
	{
		Res->SetArrayField(TEXT("edges"), EdgesArr);
	}
	return NA_SuccessObj(Res);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetCustomHLSLText(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	FString NodeGuidStr = Params->HasField(TEXT("node_guid")) ? Params->GetStringField(TEXT("node_guid")) : FString();

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script) return FMonolithActionResult::Error(TEXT("Failed to load script"));

	UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Src || !Src->NodeGraph) return FMonolithActionResult::Error(TEXT("No graph available"));

	FGuid TargetGuid;
	const bool bHasGuid = !NodeGuidStr.IsEmpty() && FGuid::Parse(NodeGuidStr, TargetGuid);
	if (!NodeGuidStr.IsEmpty() && !bHasGuid)
		return FMonolithActionResult::Error(TEXT("Invalid node_guid GUID"));

	TArray<UNiagaraNodeCustomHlsl*> HlslNodes;
	Src->NodeGraph->GetNodesOfClass<UNiagaraNodeCustomHlsl>(HlslNodes);
	if (HlslNodes.Num() == 0)
		return FMonolithActionResult::Error(TEXT("No CustomHlsl node found in script"));

	UNiagaraNodeCustomHlsl* TargetNode = nullptr;
	if (bHasGuid)
	{
		for (UNiagaraNodeCustomHlsl* Node : HlslNodes)
		{
			if (Node && Node->NodeGuid == TargetGuid)
			{
				TargetNode = Node;
				break;
			}
		}
		if (!TargetNode)
			return FMonolithActionResult::Error(FString::Printf(TEXT("CustomHlsl node '%s' not found"), *NodeGuidStr));
	}
	else if (HlslNodes.Num() == 1)
	{
		TargetNode = HlslNodes[0];
	}
	else
	{
		TArray<FString> NodeIds;
		for (UNiagaraNodeCustomHlsl* Node : HlslNodes)
		{
			if (Node) NodeIds.Add(Node->NodeGuid.ToString());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Script contains multiple CustomHlsl nodes. Pass node_guid. Available: [%s]"),
			*FString::Join(NodeIds, TEXT(", "))));
	}

	// Read the CustomHlsl UPROPERTY via reflection. UNiagaraNodeCustomHlsl::GetCustomHlsl() is
	// access-public but NOT DLL-exported (no NIAGARAEDITOR_API), so calling it cross-module fails
	// to link (LNK2019). The CustomHlsl field is a reflectable UPROPERTY, so reflection works
	// across the module boundary without an exported symbol.
	FStrProperty* HlslProp = CastField<FStrProperty>(TargetNode->GetClass()->FindPropertyByName(TEXT("CustomHlsl")));
	if (!HlslProp)
		return FMonolithActionResult::Error(TEXT("CustomHlsl property not found on CustomHlsl node"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("script_path"), ScriptPath);
	R->SetStringField(TEXT("node_guid"), TargetNode->NodeGuid.ToString());
	R->SetStringField(TEXT("hlsl"), HlslProp->GetPropertyValue_InContainer(TargetNode));
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetCustomHLSLText(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	FString HlslText = Params->GetStringField(TEXT("hlsl"));
	FString NodeGuidStr = Params->HasField(TEXT("node_guid")) ? Params->GetStringField(TEXT("node_guid")) : FString();

	if (HlslText.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required field: hlsl"));

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script) return FMonolithActionResult::Error(TEXT("Failed to load script"));

	UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Src || !Src->NodeGraph) return FMonolithActionResult::Error(TEXT("No graph available"));

	FGuid TargetGuid;
	const bool bHasGuid = !NodeGuidStr.IsEmpty() && FGuid::Parse(NodeGuidStr, TargetGuid);
	if (!NodeGuidStr.IsEmpty() && !bHasGuid)
		return FMonolithActionResult::Error(TEXT("Invalid node_guid GUID"));

	TArray<UNiagaraNodeCustomHlsl*> HlslNodes;
	Src->NodeGraph->GetNodesOfClass<UNiagaraNodeCustomHlsl>(HlslNodes);
	if (HlslNodes.Num() == 0)
		return FMonolithActionResult::Error(TEXT("No CustomHlsl node found in script"));

	UNiagaraNodeCustomHlsl* TargetNode = nullptr;
	if (bHasGuid)
	{
		for (UNiagaraNodeCustomHlsl* Node : HlslNodes)
		{
			if (Node && Node->NodeGuid == TargetGuid)
			{
				TargetNode = Node;
				break;
			}
		}
		if (!TargetNode)
			return FMonolithActionResult::Error(FString::Printf(TEXT("CustomHlsl node '%s' not found"), *NodeGuidStr));
	}
	else if (HlslNodes.Num() == 1)
	{
		TargetNode = HlslNodes[0];
	}
	else
	{
		TArray<FString> NodeIds;
		for (UNiagaraNodeCustomHlsl* Node : HlslNodes)
		{
			if (Node) NodeIds.Add(Node->NodeGuid.ToString());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Script contains multiple CustomHlsl nodes. Pass node_guid. Available: [%s]"),
			*FString::Join(NodeIds, TEXT(", "))));
	}

	// Write the CustomHlsl UPROPERTY via reflection. UNiagaraNodeCustomHlsl::SetCustomHlsl() is
	// access-public but NOT DLL-exported (no NIAGARAEDITOR_API), so calling it cross-module fails
	// to link (LNK2019). The CustomHlsl field is a reflectable UPROPERTY, so reflection works
	// across the module boundary without an exported symbol. Resolve before the transaction so a
	// missing-property failure bails cleanly without opening an empty transaction.
	FStrProperty* HlslProp = CastField<FStrProperty>(TargetNode->GetClass()->FindPropertyByName(TEXT("CustomHlsl")));
	if (!HlslProp)
		return FMonolithActionResult::Error(TEXT("CustomHlsl property not found on CustomHlsl node"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetCustomHlslText", "Set Custom HLSL Text"));
	Script->Modify();
	Src->NodeGraph->Modify();
	TargetNode->Modify();
	HlslProp->SetPropertyValue_InContainer(TargetNode, HlslText);
	TargetNode->MarkNodeRequiresSynchronization(TEXT("MonolithSetCustomHlslText"), true);
	GEditor->EndTransaction();

	Script->MarkPackageDirty();
	Script->RequestCompile(Script->GetExposedVersion().VersionGuid, false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("script_path"), ScriptPath);
	R->SetStringField(TEXT("node_guid"), TargetNode->NodeGuid.ToString());
	R->SetNumberField(TEXT("length"), HlslText.Len());
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ScriptUsage = Params->GetStringField(TEXT("usage"));
	FString ModuleScriptPath = Params->GetStringField(TEXT("module_script"));
	FString ModuleScriptBaseName = FPaths::GetBaseFilename(ModuleScriptPath);
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
			TEXT("Unrecognized usage '%s'. Valid values: system_spawn, system_update, emitter_spawn, emitter_update, particle_spawn (or spawn), particle_update (or update), particle_event (or event), particle_simulation_stage (or simulation_stage)"),
			*ScriptUsage));
	}

	FGuid UsageId;
	FString StageName;
	FString EventName;
	if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
	{
		FString StageError;
		if (!ResolveSimulationStageSelector(System, EmitterHandleId, Params, UsageId, &StageName, &StageError))
		{
			return FMonolithActionResult::Error(StageError);
		}
	}
	else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
	{
		FString EventError;
		if (!ResolveEventHandlerSelector(System, EmitterHandleId, Params, UsageId, &EventName, &EventError))
		{
			return FMonolithActionResult::Error(EventError);
		}
	}

	UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId, Usage, UsageId);
	if (!OutputNode)
	{
		if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript || Usage == ENiagaraScriptUsage::ParticleEventScript)
		{
			UNiagaraGraph* Graph = GetGraphForUsage(System, EmitterHandleId, Usage);
			TArray<FString> OutputSummaries;
			if (Graph)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UNiagaraNodeOutput* OutNode = Cast<UNiagaraNodeOutput>(Node);
					if (!OutNode) continue;
					OutputSummaries.Add(FString::Printf(TEXT("usage=%s usage_id=%s"),
						*UsageToString(OutNode->GetUsage()), *OutNode->GetUsageId().ToString()));
				}
			}

			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No output node for selector-based stage. emitter='%s' usage='%s' selector_name='%s' usage_id='%s' graph_found=%s graph_outputs=[%s]"),
				*EmitterHandleId,
				*UsageToString(Usage),
				*(Usage == ENiagaraScriptUsage::ParticleEventScript ? EventName : StageName),
				*UsageId.ToString(),
				Graph ? TEXT("true") : TEXT("false"),
				*FString::Join(OutputSummaries, TEXT(", "))));
		}

		return FMonolithActionResult::Error(TEXT("No output node"));
	}

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

	bool bAutoEnabledPersistentIds = false;
	if (Usage == ENiagaraScriptUsage::ParticleUpdateScript && ModuleScriptBaseName.Contains(TEXT("GenerateDeathEvent")))
	{
		const int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
		if (EmitterIdx != INDEX_NONE)
		{
			FVersionedNiagaraEmitterData* TargetEmitterData = System->GetEmitterHandles()[EmitterIdx].GetEmitterData();
			if (TargetEmitterData && !TargetEmitterData->bRequiresPersistentIDs)
			{
				TSharedRef<FJsonObject> PersistentIdParams = MakeShared<FJsonObject>();
				PersistentIdParams->SetStringField(TEXT("asset_path"), SystemPath);
				PersistentIdParams->SetStringField(TEXT("emitter"), EmitterHandleId);
				PersistentIdParams->SetStringField(TEXT("property"), TEXT("requires_persistent_ids"));
				PersistentIdParams->SetBoolField(TEXT("value"), true);

				FMonolithActionResult PersistentIdResult = HandleSetEmitterProperty(PersistentIdParams);
				if (!PersistentIdResult.bSuccess)
				{
					return FMonolithActionResult::Error(FString::Printf(
						TEXT("Added GenerateDeathEvent module, but failed to enable requires_persistent_ids: %s"),
						*PersistentIdResult.ErrorMessage));
				}

				bAutoEnabledPersistentIds = true;
			}
		}
	}
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("node_guid"), NewNode->NodeGuid.ToString());
	if (Usage == ENiagaraScriptUsage::ParticleSimulationStageScript)
	{
		if (!StageName.IsEmpty()) R->SetStringField(TEXT("stage_name"), StageName);
		if (UsageId.IsValid()) R->SetStringField(TEXT("usage_id"), UsageId.ToString());
	}
	else if (Usage == ENiagaraScriptUsage::ParticleEventScript)
	{
		if (!EventName.IsEmpty()) R->SetStringField(TEXT("event_name"), EventName);
		if (UsageId.IsValid()) R->SetStringField(TEXT("usage_id"), UsageId.ToString());
	}

	if (bAutoEnabledPersistentIds)
	{
		R->SetBoolField(TEXT("auto_enabled_requires_persistent_ids"), true);
		R->SetStringField(TEXT("note"), TEXT("GenerateDeathEvent was added to Particle Update, so requires_persistent_ids was enabled automatically on the emitter."));
	}

	// Warn when adding ShapeLocation modules — they need InitializeParticle Position Mode set
	if (ModuleScriptBaseName.Contains(TEXT("ShapeLocation")))
	{
		R->SetStringField(TEXT("warning"),
			TEXT("ShapeLocation requires InitializeParticle 'Position Mode' set to 'Simulation Position'. "
			     "Call set_static_switch_value on InitializeParticle if particles fail to spawn."));
	}

	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(TEXT("Module removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleMoveModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	int32 NewIndex = static_cast<int32>(Params->GetNumberField(TEXT("new_index")));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	FGuid FoundUsageId;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage, &FoundUsageId);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterHandleId, FoundUsage, FoundUsageId);
	if (!OutputNode) return FMonolithActionResult::Error(TEXT("No output node"));

	// Build the node group array from the PM chain
	TArray<MonolithNiagaraHelpers::FStackNodeGroup> Groups;
	MonolithNiagaraHelpers::GetStackNodeGroups(*OutputNode, Groups);

	// Groups layout: [0]=input connector, [1..N]=modules, [N+1]=output node
	// Find which group contains our module as its EndNode
	int32 GroupIdx = INDEX_NONE;
	int32 ModuleCount = 0;
	for (int32 i = 1; i < Groups.Num() - 1; ++i) // skip input connector (0) and output node (last)
	{
		ModuleCount++;
		if (Groups[i].EndNode == MN)
		{
			GroupIdx = i;
		}
	}

	if (GroupIdx == INDEX_NONE)
		return FMonolithActionResult::Error(TEXT("Module not found in stack node groups"));

	// Current module index is (GroupIdx - 1) since Groups[1] = module index 0
	int32 CurIdx = GroupIdx - 1;
	NewIndex = FMath::Clamp(NewIndex, 0, ModuleCount - 1);

	if (CurIdx == NewIndex)
		return NA_SuccessStr(TEXT("Already at target index"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "MoveMod", "Move Module"));
	System->Modify();

	// Save the group we're moving
	MonolithNiagaraHelpers::FStackNodeGroup MovedGroup = Groups[GroupIdx];

	// Disconnect from current position — splice neighbors together
	MonolithNiagaraHelpers::DisconnectGroup(MovedGroup, Groups[GroupIdx - 1], Groups[GroupIdx + 1]);

	// Re-fetch groups after the chain changed
	TArray<MonolithNiagaraHelpers::FStackNodeGroup> UpdatedGroups;
	MonolithNiagaraHelpers::GetStackNodeGroups(*OutputNode, UpdatedGroups);

	// Target group index: new_index + 1 (offset by 1 because UpdatedGroups[0] is the input connector)
	// We insert AFTER TargetGroupIdx, so the moved group sits between TargetGroupIdx and TargetGroupIdx+1
	int32 TargetGroupIdx = NewIndex + 1;

	// Clamp to valid range in the updated groups (which has one fewer module group now)
	TargetGroupIdx = FMath::Clamp(TargetGroupIdx, 0, UpdatedGroups.Num() - 1);

	// Insert: wire between UpdatedGroups[TargetGroupIdx-1] and UpdatedGroups[TargetGroupIdx]
	MonolithNiagaraHelpers::ConnectGroup(MovedGroup, UpdatedGroups[TargetGroupIdx - 1], UpdatedGroups[TargetGroupIdx]);

	GEditor->EndTransaction();

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("module_guid"), MN->NodeGuid.ToString());
	R->SetNumberField(TEXT("old_index"), CurIdx);
	R->SetNumberField(TEXT("new_index"), NewIndex);
	R->SetStringField(TEXT("status"), TEXT("Module moved (overrides preserved)"));
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleEnabled(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(bEnabled ? TEXT("Module enabled") : TEXT("Module disabled"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
		if (O->HasField(TEXT("x")) && O->HasField(TEXT("y")))
		{
			// Vector types: vec2, vec3, vec4
			double X = O->GetNumberField(TEXT("x")), Y = O->GetNumberField(TEXT("y"));
			double Z = O->HasField(TEXT("z")) ? O->GetNumberField(TEXT("z")) : 0.0;
			double W = O->HasField(TEXT("w")) ? O->GetNumberField(TEXT("w")) : 0.0;
			if (O->HasField(TEXT("w"))) ValStr = FString::Printf(TEXT("%f,%f,%f,%f"), X, Y, Z, W);
			else if (O->HasField(TEXT("z"))) ValStr = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
			else ValStr = FString::Printf(TEXT("%f,%f"), X, Y);
		}
		else if (O->HasField(TEXT("r")) && O->HasField(TEXT("g")))
		{
			// Color type: LinearColor
			double R2 = O->GetNumberField(TEXT("r")), G = O->GetNumberField(TEXT("g"));
			double B = O->GetNumberField(TEXT("b")), A = O->HasField(TEXT("a")) ? O->GetNumberField(TEXT("a")) : 1.0;
			ValStr = FString::Printf(TEXT("%f,%f,%f,%f"), R2, G, B, A);
		}
		else if (O->HasField(TEXT("qx")) && O->HasField(TEXT("qy")) && O->HasField(TEXT("qz")) && O->HasField(TEXT("qw")))
		{
			// Quaternion type: quat
			double QX = O->GetNumberField(TEXT("qx")), QY = O->GetNumberField(TEXT("qy"));
			double QZ = O->GetNumberField(TEXT("qz")), QW = O->GetNumberField(TEXT("qw"));
			ValStr = FString::Printf(TEXT("%f,%f,%f,%f"), QX, QY, QZ, QW);
		}
		else if (O->HasField(TEXT("m00")))
		{
			// Matrix type: 4x4 matrix (16 values)
			// Format: m00,m01,m02,m03,m10,m11,m12,m13,m20,m21,m22,m23,m30,m31,m32,m33
			double M00 = O->GetNumberField(TEXT("m00")), M01 = O->GetNumberField(TEXT("m01"));
			double M02 = O->GetNumberField(TEXT("m02")), M03 = O->GetNumberField(TEXT("m03"));
			double M10 = O->GetNumberField(TEXT("m10")), M11 = O->GetNumberField(TEXT("m11"));
			double M12 = O->GetNumberField(TEXT("m12")), M13 = O->GetNumberField(TEXT("m13"));
			double M20 = O->GetNumberField(TEXT("m20")), M21 = O->GetNumberField(TEXT("m21"));
			double M22 = O->GetNumberField(TEXT("m22")), M23 = O->GetNumberField(TEXT("m23"));
			double M30 = O->GetNumberField(TEXT("m30")), M31 = O->GetNumberField(TEXT("m31"));
			double M32 = O->GetNumberField(TEXT("m32")), M33 = O->GetNumberField(TEXT("m33"));
			ValStr = FString::Printf(TEXT("%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f"),
				M00, M01, M02, M03, M10, M11, M12, M13, M20, M21, M22, M23, M30, M31, M32, M33);
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

	return NA_SuccessStr(FString::Printf(TEXT("Set input '%s' = '%s'"), *InputName, *ValStr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(FString::Printf(TEXT("Bound '%s' to '%s'"), *InputName, *BindingPath));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetModuleInputDI(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	FString DIDiagnostic;
	UClass* DIUClass = MonolithNiagaraHelpers::ResolveNiagaraDataInterfaceClass(DIClass, &DIDiagnostic);
	if (!DIUClass)
	{
		return FMonolithActionResult::Error(DIDiagnostic.IsEmpty() ? TEXT("DI class not found") : DIDiagnostic);
	}

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
	bool bGridApplied = false;
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

		// ColorCurve convenience: if caller provides "keys" but not "red"/"green"/"blue"/"alpha",
		// fan out the scalar keys to all four RGBA channels for uniform color curves
		if (Cast<UNiagaraDataInterfaceColorCurve>(DIInst) && DIConfig->HasField(TEXT("keys"))
			&& !DIConfig->HasField(TEXT("red")))
		{
			TArray<TSharedPtr<FJsonValue>> FloatKeys = DIConfig->GetArrayField(TEXT("keys"));
			DIConfig->SetField(TEXT("red"), MakeShared<FJsonValueArray>(FloatKeys));
			DIConfig->SetField(TEXT("green"), MakeShared<FJsonValueArray>(FloatKeys));
			DIConfig->SetField(TEXT("blue"), MakeShared<FJsonValueArray>(FloatKeys));
			DIConfig->SetField(TEXT("alpha"), MakeShared<FJsonValueArray>(FloatKeys));
		}

		// Try curve-specific config first (handles keys, red/green/blue/alpha, x/y/z/w)
		bool bCurveApplied = MonolithNiagaraHelpers::ApplyCurveConfig(DIInst, DIConfig);
		bCurveConfigApplied = bCurveApplied;

		// Try grid-specific config (handles Grid2D, Grid3D, NeighborGrid3D)
		if (!bCurveApplied)
		{
			bGridApplied = MonolithNiagaraHelpers::ApplyGridConfig(DIInst, DIConfig);
		}

		// Check if config had curve-like fields that ApplyCurveConfig didn't handle
		if (!bCurveApplied && !bGridApplied)
		{
			static const TSet<FString> CurveFieldNames = {
				TEXT("keys"), TEXT("curve"),
				TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha"),
				TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")
			};
			bool bHadCurveFields = false;
			for (const auto& Pair : DIConfig->Values)
			{
				if (CurveFieldNames.Contains(MonolithKeyToString(Pair.Key))) { bHadCurveFields = true; break; }
			}
			if (bHadCurveFields)
			{
				UE_LOG(LogMonolithNiagara, Warning, TEXT("set_module_input_di: config had curve fields but DI type '%s' didn't match any known curve DI. Curve keys were NOT applied."), *DIUClass->GetName());
			}
		}

		// Fall back to simple property reflection for non-curve/non-grid properties
		// Only skip curve field names if the DI is actually a curve type — non-curve DIs
		// might legitimately have properties named "x", "y", "red", etc.
		// Similarly, skip grid field names if the DI is a grid type.
		const bool bIsCurveDI = Cast<UNiagaraDataInterfaceCurveBase>(DIInst) != nullptr;
		const bool bIsGridDI = Cast<UNiagaraDataInterfaceGrid2DCollection>(DIInst) != nullptr
			|| Cast<UNiagaraDataInterfaceGrid3DCollection>(DIInst) != nullptr
			|| Cast<UNiagaraDataInterfaceNeighborGrid3D>(DIInst) != nullptr;

		static const TSet<FString> CurveKeys = {
			TEXT("keys"), TEXT("curve"),
			TEXT("red"), TEXT("green"), TEXT("blue"), TEXT("alpha"),
			TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")
		};
		static const TSet<FString> GridKeys = {
			TEXT("num_cells_x"), TEXT("num_cells_y"), TEXT("num_cells_z"),
			TEXT("num_cells_max_axis"), TEXT("num_attributes"), TEXT("world_bbox_size"),
			TEXT("set_grid_from_max_axis"), TEXT("set_resolution_method"),
			TEXT("max_neighbors_per_cell"), TEXT("cell_size"),
			TEXT("clear_before_non_iteration_stage")
		};

		for (auto& Pair : DIConfig->Values)
		{
			if (bIsCurveDI && CurveKeys.Contains(MonolithKeyToString(Pair.Key))) continue;
			if (bIsGridDI && GridKeys.Contains(MonolithKeyToString(Pair.Key))) continue;

			FProperty* Prop = DIUClass->FindPropertyByName(FName(*MonolithKeyToString(Pair.Key)));
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
		FString ConfigMsg;
		if (bCurveConfigApplied)
		{
			ConfigMsg = TEXT(" (curve config applied)");
		}
		else if (bGridApplied)
		{
			ConfigMsg = TEXT(" (grid config applied)");
		}
		else
		{
			ConfigMsg = TEXT(" (config applied)");
		}
		return NA_SuccessStr(FString::Printf(TEXT("DI '%s' set on input '%s'%s"),
			*DIUClass->GetName(), *InputName, *ConfigMsg));
	}
	return NA_SuccessStr(FString::Printf(TEXT("DI '%s' set on input '%s'"), *DIUClass->GetName(), *InputName));
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

	// Validate the destination up front — a malformed path asserts inside CreatePackage below.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

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
		bool bTypeFellBack = false;
		FNiagaraTypeDefinition ResolvedType = ResolveNiagaraType(TypeStr, &bTypeFellBack);
		if (bTypeFellBack)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown Niagara input type '%s' for pin '%s'. Use a real Niagara type name (e.g. float, vec3, NeighborGrid3D, ParticleRead) instead of relying on implicit fallback."),
				*TypeStr, *PinName));
		}
		ParsedInputs.Add({ PinName, ResolvedType });
	}

	for (const TSharedPtr<FJsonValue>& Val : GetJsonArray(Params, TEXT("outputs")))
	{
		TSharedPtr<FJsonObject> Obj = AsObjectOrParseString(Val);
		if (!Obj.IsValid() || Obj->Values.Num() == 0) continue;
		FString PinName = Obj->GetStringField(TEXT("name"));
		FString TypeStr = Obj->GetStringField(TEXT("type"));
		if (PinName.IsEmpty() || TypeStr.IsEmpty()) continue;
		bool bTypeFellBack = false;
		FNiagaraTypeDefinition ResolvedType = ResolveNiagaraType(TypeStr, &bTypeFellBack);
		if (bTypeFellBack)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown Niagara output type '%s' for pin '%s'. Use a real Niagara type name instead of relying on implicit fallback."),
				*TypeStr, *PinName));
		}
		ParsedOutputs.Add({ PinName, ResolvedType });
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
	const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);

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
		OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
		for (const FPinDef& Output : ParsedOutputs)
		{
			OutputNode->Outputs.Add(FNiagaraVariable(Output.Type, FName(*Output.Name)));
		}
	}
	else if (ParsedOutputs.Num() > 0)
	{
		OutputNode->Outputs.Add(FNiagaraVariable(ParsedOutputs[0].Type, FName(*ParsedOutputs[0].Name)));
	}
	else
	{
		OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Output")));
	}
	OutputCreator.Finalize();

	// --- InputNode ---
	FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
	UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
	InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
	if (bIsModule)
	{
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		InputNode->ExposureOptions.bExposed = false;
		InputNode->ExposureOptions.bRequired = false;
		InputNode->ExposureOptions.bHidden = true;
		InputNode->ExposureOptions.bCanAutoBind = true;
	}
	else if (ParsedInputs.Num() > 0)
	{
		InputNode->Input = FNiagaraVariable(ParsedInputs[0].Type, FName(*ParsedInputs[0].Name));
	}
	else
	{
		InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Input"));
	}
	InputCreator.Finalize();

	// For Function usage, create additional typed input nodes for each parameter beyond the first.
	// Module usage does NOT need these — it reads all inputs via ParameterMapGet.
	TMap<FName, UNiagaraNodeInput*> TypedInputNodesByName;
	if (!bIsModule)
	{
		for (int32 i = 1; i < ParsedInputs.Num(); ++i)
		{
			const FPinDef& Input = ParsedInputs[i];

			FGraphNodeCreator<UNiagaraNodeInput> TypedInputCreator(*Graph);
			UNiagaraNodeInput* TypedInputNode = TypedInputCreator.CreateNode();
			TypedInputNode->Usage = ENiagaraInputNodeUsage::Parameter;
			TypedInputNode->Input = FNiagaraVariable(Input.Type, FName(*Input.Name));
			TypedInputNode->CallSortPriority = i;
			TypedInputCreator.Finalize();
			TypedInputNodesByName.Add(FName(*Input.Name), TypedInputNode);
		}
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

#if WITH_NIAGARA_WIZARD_PRIVATE
	// Module ParameterMap bridge requires the engine-private Wizard::Utilities::Add*ParameterPin
	// helpers (gated — see the forward-decl breadcrumb near the top of this file). When the gate
	// is off, the #else branch falls back to the strict typed-pin wiring instead.
	UNiagaraNodeParameterMapGet* MapGetNode = nullptr;
	UNiagaraNodeParameterMapSet* MapSetNode = nullptr;
	if (bIsModule)
	{
		FGraphNodeCreator<UNiagaraNodeParameterMapGet> MapGetCreator(*Graph);
		MapGetNode = MapGetCreator.CreateNode(false);
		MapGetCreator.Finalize();

		FGraphNodeCreator<UNiagaraNodeParameterMapSet> MapSetCreator(*Graph);
		MapSetNode = MapSetCreator.CreateNode(false);
		MapSetCreator.Finalize();
	}

	// === Wire the graph ===
	if (bIsModule)
	{
		check(MapGetNode && MapSetNode);

		UEdGraphPin* InputMapOut = InputNode->GetOutputPin(0);
		UEdGraphPin* MapGetMapIn = MapGetNode->GetInputPin(0);
		UEdGraphPin* MapSetMapIn = MapSetNode->GetInputPin(0);
		UEdGraphPin* MapSetMapOut = MapSetNode->GetOutputPin(0);
		UEdGraphPin* HlslMapIn = MonolithNiagaraHelpers::GetParameterMapPin(*HlslNode, EGPD_Input);
		UEdGraphPin* HlslMapOut = MonolithNiagaraHelpers::GetParameterMapPin(*HlslNode, EGPD_Output);
		UEdGraphPin* OutputMapIn = OutputNode->GetInputPin(0);

		if (InputMapOut && MapGetMapIn)
			Schema->TryCreateConnection(InputMapOut, MapGetMapIn);
		if (InputMapOut && HlslMapIn)
			Schema->TryCreateConnection(InputMapOut, HlslMapIn);
		if (HlslMapOut && MapSetMapIn)
			Schema->TryCreateConnection(HlslMapOut, MapSetMapIn);
		if (MapSetMapOut && OutputMapIn)
			Schema->TryCreateConnection(MapSetMapOut, OutputMapIn);

		TMap<FName, UEdGraphPin*> MapGetPinsByShortName;
		for (const FPinDef& Input : ParsedInputs)
		{
			const FName ModuleInputName(*FString::Printf(TEXT("Module.%s"), *Input.Name));
			UEdGraphPin* GetPin = UE::Niagara::Wizard::Utilities::AddReadParameterPin(Input.Type, ModuleInputName, MapGetNode);
			if (GetPin)
			{
				MapGetPinsByShortName.Add(FName(*Input.Name), GetPin);
			}
		}

		TMap<FName, UEdGraphPin*> MapSetPinsByShortName;
		for (const FPinDef& Output : ParsedOutputs)
		{
			const FName OutputName(*FString::Printf(TEXT("Output.%s"), *Output.Name));
			UEdGraphPin* SetPin = UE::Niagara::Wizard::Utilities::AddWriteParameterPin(Output.Type, OutputName, MapSetNode);
			if (SetPin)
			{
				MapSetPinsByShortName.Add(FName(*Output.Name), SetPin);
			}
		}

		for (UEdGraphPin* HlslPin : HlslNode->Pins)
		{
			if (HlslPin->PinName.IsNone() || HlslPin->PinName == TEXT("Add")) continue;
			if (Schema->PinToTypeDefinition(HlslPin) == FNiagaraTypeDefinition::GetParameterMapDef()) continue;

			if (HlslPin->Direction == EGPD_Input)
			{
				if (UEdGraphPin* const* GetPin = MapGetPinsByShortName.Find(HlslPin->PinName))
				{
					Schema->TryCreateConnection(*GetPin, HlslPin);
				}
			}
			else if (HlslPin->Direction == EGPD_Output)
			{
				if (UEdGraphPin* const* SetPin = MapSetPinsByShortName.Find(HlslPin->PinName))
				{
					Schema->TryCreateConnection(HlslPin, *SetPin);
				}

				for (UEdGraphPin* OutPin : OutputNode->Pins)
				{
					if (OutPin->Direction == EGPD_Input && OutPin->PinName == HlslPin->PinName)
					{
						Schema->TryCreateConnection(HlslPin, OutPin);
						break;
					}
				}
			}
		}
	}
#else // WITH_NIAGARA_WIZARD_PRIVATE
	// === Wire the graph (fallback: strict typed-pin path, no engine-private ParameterMap bridge) ===
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
#endif // WITH_NIAGARA_WIZARD_PRIVATE
	if (!bIsModule)
	{
		// Function wiring: connect typed input nodes to HlslNode inputs, and HlslNode outputs to OutputNode
		UEdGraphPin* OutputIn = OutputNode->GetInputPin(0);

		// Connect the first input node to the first HlslNode input pin
		UEdGraphPin* FirstInputOut = InputNode->GetOutputPin(0);
		if (FirstInputOut)
		{
			for (UEdGraphPin* Pin : HlslNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && !Pin->PinName.IsNone()
					&& Pin->PinName != TEXT("Add"))
				{
					Schema->TryCreateConnection(FirstInputOut, Pin);
					break;
				}
			}
		}

		// Connect additional typed input nodes (from TypedInputNodesByName) to their corresponding HlslNode pins
		for (const auto& Pair : TypedInputNodesByName)
		{
			UNiagaraNodeInput* TypedInputNode = Pair.Value;
			UEdGraphPin* TypedInputOut = TypedInputNode->GetOutputPin(0);
			if (!TypedInputOut) continue;

			for (UEdGraphPin* HlslPin : HlslNode->Pins)
			{
				if (HlslPin->Direction == EGPD_Input && HlslPin->PinName == Pair.Key)
				{
					Schema->TryCreateConnection(TypedInputOut, HlslPin);
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

	return NA_SuccessObj(Result);
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
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessStr(JsonArrayToString(All));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetUserParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Use ReadParameterVariables (live store entries) instead of GetUserParameters()
	// which the engine warns returns STALE redirect-map keys.
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<TSharedPtr<FJsonValue>> Arr;
	CollectParametersFromStore(US, TEXT("User"), Arr);
	return NA_SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetParameterValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
			return NA_SuccessObj(R);
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

	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleTraceParameterBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
		return NA_SuccessObj(Trace);
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
	return NA_SuccessObj(Trace);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddUserParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	// Accept "name" (canonical) or "parameter_name" (common alias)
	FString ParamName = Params->HasField(TEXT("name")) ? Params->GetStringField(TEXT("name")) : Params->GetStringField(TEXT("parameter_name"));
	FString TypeName = Params->GetStringField(TEXT("type"));
	// Accept "default" (canonical) or "default_value" (common alias)
	TSharedPtr<FJsonValue> DefaultJV = Params->HasField(TEXT("default")) ? Params->TryGetField(TEXT("default"))
		: Params->HasField(TEXT("default_value")) ? Params->TryGetField(TEXT("default_value")) : TSharedPtr<FJsonValue>();

	if (ParamName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Parameter name is required — pass as \"name\" field"));

	if (TypeName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Parameter type is required — pass as \"type\" field. Valid types: float, int, bool, vec2, vec3, vec4, color, position, quat, matrix"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	bool bTypeFellBack = false;
	FNiagaraTypeDefinition TD = ResolveNiagaraType(TypeName, &bTypeFellBack);
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

	// --- Readback verification: confirm parameter was actually added to the store ---
	{
		TArray<FNiagaraVariable> VerifyUP;
		US.GetUserParameters(VerifyUP);
		FString VerifySearch = ParamName;
		if (VerifySearch.StartsWith(TEXT("User."))) VerifySearch = VerifySearch.Mid(5);
		bool bFoundInVerify = false;
		FNiagaraTypeDefinition FoundType;
		for (const FNiagaraVariable& VP : VerifyUP)
		{
			if (VP.GetName().ToString().Equals(VerifySearch, ESearchCase::IgnoreCase))
			{
				bFoundInVerify = true;
				FoundType = VP.GetType();
				break;
			}
		}
		if (!bFoundInVerify)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AddParameter succeeded but parameter '%s' not found in store readback — possible engine cascade removed it"), *ParamName));
		}
		if (FoundType != TD)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("AddParameter type mismatch: requested '%s' but store contains '%s' for parameter '%s'"),
				*TD.GetName(), *FoundType.GetName(), *ParamName));
		}
	}

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

				// Value readback for historically problematic types (Bool, Vec3f)
				if (TD == FNiagaraTypeDefinition::GetBoolDef())
				{
					FNiagaraBool ReadBack = US.GetParameterValue<FNiagaraBool>(StoreVar);
					if (ReadBack.GetValue() != NV.GetValue<FNiagaraBool>().GetValue())
					{
						UE_LOG(LogMonolithNiagara, Warning, TEXT("add_user_parameter: Bool value readback mismatch for '%s' — wrote %s, read %s (editor cascade may overwrite)"),
							*ParamName,
							NV.GetValue<FNiagaraBool>().GetValue() ? TEXT("true") : TEXT("false"),
							ReadBack.GetValue() ? TEXT("true") : TEXT("false"));
					}
				}
				else if (TD == FNiagaraTypeDefinition::GetVec3Def() || TD == FNiagaraTypeDefinition::GetPositionDef())
				{
					FVector3f ReadBack = US.GetParameterValue<FVector3f>(StoreVar);
					FVector3f Expected = NV.GetValue<FVector3f>();
					if (!ReadBack.Equals(Expected, KINDA_SMALL_NUMBER))
					{
						UE_LOG(LogMonolithNiagara, Warning, TEXT("add_user_parameter: Vec3f value readback mismatch for '%s' — wrote (%f,%f,%f), read (%f,%f,%f) (editor cascade may overwrite)"),
							*ParamName, Expected.X, Expected.Y, Expected.Z, ReadBack.X, ReadBack.Y, ReadBack.Z);
					}
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

	// Build response with resolved_type for caller visibility
	FString ResolvedTypeName = TD.GetName();
	TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("resolved_type"), ResolvedTypeName);

	if (bTypeFellBack)
	{
		ResultObj->SetStringField(TEXT("warning"), FString::Printf(TEXT("Unknown type '%s' — defaulted to float. Valid types: float, int, bool, vec2, vec3, vec4, color, position, quat, matrix"), *TypeName));
	}

	if (DefaultJV.IsValid() && !bDefaultSet)
	{
		ResultObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Added user parameter '%s' but failed to set default value — check value format matches type '%s'"), *ParamName, *TypeName));
		return NA_SuccessObj(ResultObj);
	}
	ResultObj->SetStringField(TEXT("message"), FString::Printf(TEXT("Added user parameter '%s'%s"), *ParamName, bDefaultSet ? TEXT(" with default") : TEXT("")));
	return NA_SuccessObj(ResultObj);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveUserParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
			return NA_SuccessStr(FString::Printf(TEXT("Removed parameter '%s'"), *ParamName));
		}
	}
	return FMonolithActionResult::Error(TEXT("Parameter not found"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetParameterDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return bOk ? NA_SuccessStr(TEXT("Default set")) : FMonolithActionResult::Error(TEXT("Unsupported type"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetCurveValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
			TEXT("Input '%s' is a DataInterface type. Use configure_curve_keys (preferred) or set_module_input_di with di_class: \"%s\" instead. Example: {\"op\": \"configure_curve_keys\", \"input\": \"%s\", \"config\": {\"keys\": [...]}}"),
			*InputName, *DIClassName, *InputName));
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

	return NA_SuccessStr(FString::Printf(TEXT("Set curve with %d keys"), Keys.Num()));
}

// ============================================================================
// Renderer Actions (6)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveRenderer(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(TEXT("Renderer removed"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString MaterialPath = Params->GetStringField(TEXT("material"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	// Allow empty/"none" to clear the material slot
	const bool bClear = MaterialPath.IsEmpty() || MaterialPath.Equals(TEXT("none"), ESearchCase::IgnoreCase);
	UMaterialInterface* Mat = nullptr;
	if (!bClear)
	{
		Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (!Mat) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material '%s'"), *MaterialPath));
	}

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
	return bOk ? NA_SuccessStr(bClear ? TEXT("Material cleared") : TEXT("Material set")) : FMonolithActionResult::Error(TEXT("Unsupported renderer type"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return bOk ? NA_SuccessStr(TEXT("Property set")) : FMonolithActionResult::Error(TEXT("Failed to set property"));
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetRendererBindings(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessStr(JsonArrayToString(Arr));
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererBinding(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return bOk ? NA_SuccessStr(TEXT("Binding set")) : FMonolithActionResult::Error(TEXT("Failed to set binding"));
}

// ============================================================================
// Batch Actions (2)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleBatchExecute(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);

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

	// Track whether any write ops are in the batch — skip transaction/compile for read-only batches
	static const TArray<FString> WritePrefixes = {
		TEXT("add_"), TEXT("remove_"), TEXT("set_"), TEXT("move_"), TEXT("create_"),
		TEXT("configure_"), TEXT("duplicate_"), TEXT("reorder_"), TEXT("request_compile"), TEXT("rename_")
	};
	auto IsWriteOp = [](const FString& Name) -> bool
	{
		for (const FString& Prefix : WritePrefixes)
		{
			if (Name.StartsWith(Prefix)) return true;
		}
		return false;
	};

	bool bAnyWrites = false;
	// Pre-scan to detect writes
	for (int32 i = 0; i < Ops.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Op = Ops[i]->AsObject();
		if (!Op.IsValid()) continue;
		FString OpName = Op->GetStringField(TEXT("op"));
		if (OpName.IsEmpty()) OpName = Op->GetStringField(TEXT("action"));
		if (OpName.IsEmpty()) OpName = Op->GetStringField(TEXT("operation"));
		if (IsWriteOp(OpName)) { bAnyWrites = true; break; }
	}

	if (bAnyWrites)
	{
		GEditor->BeginTransaction(NSLOCTEXT("Monolith", "BatchExec", "Batch Execute"));
		System->Modify();
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 Ok = 0, Fail = 0;

	for (int32 i = 0; i < Ops.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Op = Ops[i]->AsObject();
		if (!Op.IsValid()) { Fail++; continue; }

		FString OpName = Op->GetStringField(TEXT("op"));
		if (OpName.IsEmpty()) OpName = Op->GetStringField(TEXT("action"));
		if (OpName.IsEmpty()) OpName = Op->GetStringField(TEXT("operation"));
		if (OpName.IsEmpty())
		{
			Fail++;
			auto FailObj = MakeShared<FJsonObject>();
			FailObj->SetBoolField(TEXT("success"), false);
			FailObj->SetNumberField(TEXT("index"), i);
			FailObj->SetStringField(TEXT("error"), TEXT("Missing 'op' field in operation object. Use 'op' as the key name."));
			Results.Add(MakeShared<FJsonValueObject>(FailObj));
			continue;
		}
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
		else if (OpName == TEXT("get_module_inputs")) SubResult = HandleGetModuleInputs(SubParams);
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
		// Phase 3: Dynamic Input Features
		else if (OpName == TEXT("list_dynamic_inputs")) SubResult = HandleListDynamicInputs(SubParams);
		else if (OpName == TEXT("get_dynamic_input_tree")) SubResult = HandleGetDynamicInputTree(SubParams);
		else if (OpName == TEXT("remove_dynamic_input")) SubResult = HandleRemoveDynamicInput(SubParams);
		else if (OpName == TEXT("get_dynamic_input_value")) SubResult = HandleGetDynamicInputValue(SubParams);
		else if (OpName == TEXT("get_dynamic_input_inputs")) SubResult = HandleGetDynamicInputInputs(SubParams);
		// Wave 6
		else if (OpName == TEXT("add_event_handler")) SubResult = HandleAddEventHandler(SubParams);
		else if (OpName == TEXT("validate_system")) SubResult = HandleValidateSystem(SubParams);
		else if (OpName == TEXT("add_simulation_stage")) SubResult = HandleAddSimulationStage(SubParams);
		// Composite
		else if (OpName == TEXT("set_spawn_shape")) SubResult = HandleSetSpawnShape(SubParams);
		// Phase 4: Module & Emitter Management
		else if (OpName == TEXT("rename_emitter")) SubResult = HandleRenameEmitter(SubParams);
		else if (OpName == TEXT("get_emitter_property")) SubResult = HandleGetEmitterProperty(SubParams);
		// Phase 5: Renderer & DI Improvements
		else if (OpName == TEXT("list_available_renderers")) SubResult = HandleListAvailableRenderers(SubParams);
		else if (OpName == TEXT("set_renderer_mesh")) SubResult = HandleSetRendererMesh(SubParams);
		else if (OpName == TEXT("configure_ribbon")) SubResult = HandleConfigureRibbon(SubParams);
		else if (OpName == TEXT("configure_subuv")) SubResult = HandleConfigureSubUV(SubParams);
		// Phase 6A: Event Handlers, Simulation Stages, Module Outputs
		else if (OpName == TEXT("get_event_handlers")) SubResult = HandleGetEventHandlers(SubParams);
		else if (OpName == TEXT("set_event_handler_property")) SubResult = HandleSetEventHandlerProperty(SubParams);
		else if (OpName == TEXT("remove_event_handler")) SubResult = HandleRemoveEventHandler(SubParams);
		else if (OpName == TEXT("get_simulation_stages")) SubResult = HandleGetSimulationStages(SubParams);
		else if (OpName == TEXT("set_simulation_stage_property")) SubResult = HandleSetSimulationStageProperty(SubParams);
		else if (OpName == TEXT("remove_simulation_stage")) SubResult = HandleRemoveSimulationStage(SubParams);
		else if (OpName == TEXT("get_module_output_parameters")) SubResult = HandleGetModuleOutputParameters(SubParams);
		// Phase 6B: NPC Support
		else if (OpName == TEXT("create_npc")) SubResult = HandleCreateNPC(SubParams);
		else if (OpName == TEXT("get_npc")) SubResult = HandleGetNPC(SubParams);
		else if (OpName == TEXT("add_npc_parameter")) SubResult = HandleAddNPCParameter(SubParams);
		else if (OpName == TEXT("remove_npc_parameter")) SubResult = HandleRemoveNPCParameter(SubParams);
		else if (OpName == TEXT("set_npc_default")) SubResult = HandleSetNPCDefault(SubParams);
		// Phase 6B: Effect Type CRUD
		else if (OpName == TEXT("create_effect_type")) SubResult = HandleCreateEffectType(SubParams);
		else if (OpName == TEXT("get_effect_type")) SubResult = HandleGetEffectType(SubParams);
		else if (OpName == TEXT("set_effect_type_property")) SubResult = HandleSetEffectTypeProperty(SubParams);
		// Phase 6B: Parameter Discovery
		else if (OpName == TEXT("get_available_parameters")) SubResult = HandleGetAvailableParameters(SubParams);
		// Phase 6B: Preview
		else if (OpName == TEXT("preview_system")) SubResult = HandlePreviewSystem(SubParams);
		// Phase 7: Advanced Features
		else if (OpName == TEXT("diff_systems")) SubResult = HandleDiffSystems(SubParams);
		else if (OpName == TEXT("save_emitter_as_template")) SubResult = HandleSaveEmitterAsTemplate(SubParams);
		else if (OpName == TEXT("clone_module_overrides")) SubResult = HandleCloneModuleOverrides(SubParams);
		// Read operations (14)
		else if (OpName == TEXT("get_ordered_modules")) SubResult = HandleGetOrderedModules(SubParams);
		else if (OpName == TEXT("get_all_parameters")) SubResult = HandleGetAllParameters(SubParams);
		else if (OpName == TEXT("get_user_parameters")) SubResult = HandleGetUserParameters(SubParams);
		else if (OpName == TEXT("get_parameter_value")) SubResult = HandleGetParameterValue(SubParams);
		else if (OpName == TEXT("get_parameter_type")) SubResult = HandleGetParameterType(SubParams);
		else if (OpName == TEXT("trace_parameter_binding")) SubResult = HandleTraceParameterBinding(SubParams);
		else if (OpName == TEXT("get_renderer_bindings")) SubResult = HandleGetRendererBindings(SubParams);
		else if (OpName == TEXT("list_emitters")) SubResult = HandleListEmitters(SubParams);
		else if (OpName == TEXT("list_renderers")) SubResult = HandleListRenderers(SubParams);
		else if (OpName == TEXT("list_renderer_properties")) SubResult = HandleListRendererProperties(SubParams);
		else if (OpName == TEXT("list_module_scripts")) SubResult = HandleListModuleScripts(SubParams);
		else if (OpName == TEXT("get_module_graph")) SubResult = HandleGetModuleGraph(SubParams);
		else if (OpName == TEXT("get_custom_hlsl_text")) SubResult = HandleGetCustomHLSLText(SubParams);
		else if (OpName == TEXT("set_custom_hlsl_text")) SubResult = HandleSetCustomHLSLText(SubParams);
		else if (OpName == TEXT("get_di_functions")) SubResult = HandleGetDIFunctions(SubParams);
		else if (OpName == TEXT("get_compiled_gpu_hlsl")) SubResult = HandleGetCompiledGPUHLSL(SubParams);
		// Phase 8: Expansion
		else if (OpName == TEXT("save_system")) SubResult = HandleSaveSystem(SubParams);
		else if (OpName == TEXT("get_static_switch_value")) SubResult = HandleGetStaticSwitchValue(SubParams);
		else if (OpName == TEXT("import_system_spec")) SubResult = HandleImportSystemSpec(SubParams);
		// Phase 9: Medium Priority Expansion
		else if (OpName == TEXT("get_di_properties")) SubResult = HandleGetDIProperties(SubParams);
		else if (OpName == TEXT("clear_emitter_modules")) SubResult = HandleClearEmitterModules(SubParams);
		else if (OpName == TEXT("get_module_script_inputs")) SubResult = HandleGetModuleScriptInputs(SubParams);
		else if (OpName == TEXT("get_scalability_settings")) SubResult = HandleGetScalabilitySettings(SubParams);
		else if (OpName == TEXT("set_scalability_settings")) SubResult = HandleSetScalabilitySettings(SubParams);
		else if (OpName == TEXT("list_systems")) SubResult = HandleListSystems(SubParams);
		// Phase 10: Low Priority & QoL
		else if (OpName == TEXT("duplicate_module")) SubResult = HandleDuplicateModule(SubParams);
		else if (OpName == TEXT("get_emitter_parent")) SubResult = HandleGetEmitterParent(SubParams);
		else if (OpName == TEXT("rename_user_parameter")) SubResult = HandleRenameUserParameter(SubParams);
		// Layout (Phase 3b)
		else if (OpName == TEXT("auto_layout")) SubResult = FMonolithNiagaraLayoutActions::HandleAutoLayout(SubParams);
		// Tranche 2 (#64): read-only Search & Discovery + per-system DI
		else if (OpName == TEXT("search_by_parameter")) SubResult = HandleSearchByParameter(SubParams);
		else if (OpName == TEXT("search_by_data_interface")) SubResult = HandleSearchByDataInterface(SubParams);
		else if (OpName == TEXT("query_niagara")) SubResult = HandleQueryNiagara(SubParams);
		else if (OpName == TEXT("find_similar_systems")) SubResult = HandleFindSimilarSystems(SubParams);
		else if (OpName == TEXT("search_by_material")) SubResult = HandleSearchByMaterial(SubParams);
		else if (OpName == TEXT("find_niagara_references")) SubResult = HandleFindNiagaraReferences(SubParams);
		else if (OpName == TEXT("list_system_data_interfaces")) SubResult = HandleListSystemDataInterfaces(SubParams);

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

	if (bAnyWrites)
	{
		GEditor->EndTransaction();
		System->RequestCompile(false);
	}

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), Fail == 0);
	Final->SetNumberField(TEXT("total"), Ops.Num());
	Final->SetNumberField(TEXT("succeeded"), Ok);
	Final->SetNumberField(TEXT("failed"), Fail);
	Final->SetArrayField(TEXT("results"), Results);
	return NA_SuccessObj(Final);
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

	// Delegate to shared helper
	TArray<FString> FailedOps;
	int32 FailCount = ApplySpecToSystem(LoadSystem(SystemPath), SystemPath, Spec, FailedOps);

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
	return NA_SuccessObj(Final);
}

// ============================================================================
// DI Actions (1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetDIFunctions(const TSharedPtr<FJsonObject>& Params)
{
	FString DIClassName = Params->GetStringField(TEXT("di_class"));
	FString DIDiagnostic;
	UClass* DIC = MonolithNiagaraHelpers::ResolveNiagaraDataInterfaceClass(DIClassName, &DIDiagnostic);
	if (!DIC)
		return FMonolithActionResult::Error(DIDiagnostic.IsEmpty() ? TEXT("DI class not found") : DIDiagnostic);

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
	return NA_SuccessStr(JsonArrayToString(Arr));
}

// ============================================================================
// HLSL Actions (1)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetCompiledGPUHLSL(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	return NA_SuccessStr(HLSL);
}

// ============================================================================
// Read Actions (2)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleListEmitters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);

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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleListRenderers(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
}

// ============================================================================
// Diagnostics (1)
// ============================================================================

// Renderer-attribute hazard detection shared by get_system_diagnostics + validate_system.
// Kept in the named MonolithNiagaraHelpers namespace (NOT an anonymous namespace) and NA_-prefixed
// to avoid the cross-TU full-unity C2011/C2086 collision class that issue #68 cleared (see 760cffa).
namespace MonolithNiagaraHelpers
{
	struct FNA_RendererAttributeHazardRule
	{
		const TCHAR* RendererClassName = nullptr;
		const TCHAR* AttributeName = nullptr;
		const TCHAR* FriendlyAttributeName = nullptr;
		const TCHAR* ConsumerToken = nullptr;
	};

	bool NA_ContainsTokenInsensitive(const FString& Haystack, const TCHAR* Needle)
	{
		return Haystack.Contains(Needle, ESearchCase::IgnoreCase);
	}

	bool NA_HasRendererClass(const TArray<UNiagaraRendererProperties*>& Renderers, const TCHAR* RendererClassName)
	{
		for (UNiagaraRendererProperties* Renderer : Renderers)
		{
			if (Renderer && Renderer->GetIsEnabled() && Renderer->GetClass()->GetName() == RendererClassName)
			{
				return true;
			}
		}
		return false;
	}

	bool NA_HasConsumerModule(const TArray<FString>& ModuleScriptPaths, const TCHAR* ConsumerToken)
	{
		for (const FString& Path : ModuleScriptPaths)
		{
			if (NA_ContainsTokenInsensitive(Path, ConsumerToken))
			{
				return true;
			}
		}
		return false;
	}

	bool NA_HasAttributeInitializationModule(const TArray<FString>& ParticleSpawnPaths, const TCHAR* FriendlyAttributeName)
	{
		for (const FString& Path : ParticleSpawnPaths)
		{
			if (NA_ContainsTokenInsensitive(Path, TEXT("InitializeParticle")))
			{
				if (NA_ContainsTokenInsensitive(Path, FriendlyAttributeName)
					|| NA_ContainsTokenInsensitive(Path, TEXT("InitializeRibbon"))
					|| NA_ContainsTokenInsensitive(Path, TEXT("InitializeMesh"))
					|| NA_ContainsTokenInsensitive(Path, TEXT("InitializeSprite")))
				{
					return true;
				}
			}
		}
		return false;
	}
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetSystemDiagnostics(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

		FString HandleId = Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
		auto CollectModuleScriptPaths = [&](ENiagaraScriptUsage Usage) -> TArray<FString>
		{
			TArray<FString> Paths;
			UNiagaraNodeOutput* OutNode = FindOutputNode(System, HandleId, Usage);
			if (OutNode)
			{
				TArray<UNiagaraNodeFunctionCall*> Modules;
				MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutNode, Modules);
				for (UNiagaraNodeFunctionCall* MN : Modules)
				{
					if (MN && MN->FunctionScript)
					{
						Paths.Add(MN->FunctionScript->GetPathName());
					}
				}
			}
			return Paths;
		};

		TArray<FString> ParticleSpawnPaths = CollectModuleScriptPaths(ENiagaraScriptUsage::ParticleSpawnScript);
		TArray<FString> ParticleUpdatePaths = CollectModuleScriptPaths(ENiagaraScriptUsage::ParticleUpdateScript);

		static const MonolithNiagaraHelpers::FNA_RendererAttributeHazardRule HazardRules[] = {
			{ TEXT("NiagaraSpriteRendererProperties"), TEXT("Particles.SpriteSize"), TEXT("SpriteSize"), TEXT("ScaleSpriteSize") },
			{ TEXT("NiagaraRibbonRendererProperties"), TEXT("Particles.RibbonWidth"), TEXT("RibbonWidth"), TEXT("Ribbon") },
			{ TEXT("NiagaraMeshRendererProperties"), TEXT("Particles.MeshScale"), TEXT("MeshScale"), TEXT("ScaleMesh") },
		};

		for (const MonolithNiagaraHelpers::FNA_RendererAttributeHazardRule& Rule : HazardRules)
		{
			if (!MonolithNiagaraHelpers::NA_HasRendererClass(ED->GetRenderers(), Rule.RendererClassName))
			{
				continue;
			}

			const bool bHasConsumer = MonolithNiagaraHelpers::NA_HasConsumerModule(ParticleSpawnPaths, Rule.ConsumerToken)
				|| MonolithNiagaraHelpers::NA_HasConsumerModule(ParticleUpdatePaths, Rule.ConsumerToken);
			if (!bHasConsumer)
			{
				continue;
			}

			const bool bHasAttributeInit = MonolithNiagaraHelpers::NA_HasAttributeInitializationModule(ParticleSpawnPaths, Rule.FriendlyAttributeName);
			if (!bHasAttributeInit)
			{
				Warnings.Add(MakeDiag(EmitterName,
					TEXT("RendererAttributeInit"),
					FString::Printf(TEXT("Renderer depends on %s, but no particle-spawn initialization for %s was detected before later modules consume it. The emitter may compile successfully but render nothing."),
						Rule.AttributeName,
						Rule.FriendlyAttributeName)));
			}
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
	return NA_SuccessObj(R);
}

// ============================================================================
// list_module_scripts — Search available Niagara module scripts via Asset Registry
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleListModuleScripts(const TSharedPtr<FJsonObject>& Params)
{
	FString Search = Params->HasField(TEXT("search")) ? Params->GetStringField(TEXT("search")) : TEXT("");
	FString UsageFilter = Params->HasField(TEXT("usage")) ? Params->GetStringField(TEXT("usage")).ToLower() : TEXT("");
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;
	bool bIncludeMetadata = Params->HasField(TEXT("include_metadata")) && Params->GetBoolField(TEXT("include_metadata"));

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Ensure engine Niagara content is scanned (lazy-loaded by default, so engine
	// modules like GravityForce won't appear until the Niagara editor triggers a scan).
	// bForceRescan=false makes subsequent calls a no-op — no performance penalty.
	AR.ScanPathsSynchronous({TEXT("/Niagara/")}, /*bForceRescan=*/false);

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

		// Phase 10: optional metadata — loads the script to extract version data
		if (bIncludeMetadata)
		{
			UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *PackagePath);
			if (Script)
			{
				const FVersionedNiagaraScriptData* SD = Script->GetScriptData(Script->GetExposedVersion().VersionGuid);
				if (SD)
				{
					Entry->SetNumberField(TEXT("module_usage_bitmask"), SD->ModuleUsageBitmask);
					// Decode bitmask into human-readable stage names
					TArray<FString> ValidStages;
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::ParticleSpawnScript))) ValidStages.Add(TEXT("particle_spawn"));
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::ParticleUpdateScript))) ValidStages.Add(TEXT("particle_update"));
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::EmitterSpawnScript))) ValidStages.Add(TEXT("emitter_spawn"));
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::EmitterUpdateScript))) ValidStages.Add(TEXT("emitter_update"));
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::SystemSpawnScript))) ValidStages.Add(TEXT("system_spawn"));
					if (SD->ModuleUsageBitmask & (1 << static_cast<int32>(ENiagaraScriptUsage::SystemUpdateScript))) ValidStages.Add(TEXT("system_update"));
					if (ValidStages.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> StagesArr;
						for (const FString& S : ValidStages) StagesArr.Add(MakeShared<FJsonValueString>(S));
						Entry->SetArrayField(TEXT("valid_stages"), StagesArr);
					}

					if (!SD->Category.IsEmpty())
						Entry->SetStringField(TEXT("category"), SD->Category.ToString());
					if (!SD->Description.IsEmpty())
						Entry->SetStringField(TEXT("description"), SD->Description.ToString());
					Entry->SetBoolField(TEXT("deprecated"), SD->bDeprecated);
					Entry->SetBoolField(TEXT("experimental"), SD->bExperimental);
					Entry->SetBoolField(TEXT("suggested"), SD->bSuggested);
				}
			}
		}

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
	return NA_SuccessObj(R);
}

// ============================================================================
// list_renderer_properties — List editable properties on a renderer
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleListRendererProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
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
	FString SystemPath = NA_GetAssetPath(Params);
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
		return NA_SuccessObj(R);
	}
	if (ResolvedName == TEXT("WarmupTickDelta"))
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("property"), ResolvedName);
		R->SetNumberField(TEXT("value"), System->GetWarmupTickDelta());
		return NA_SuccessObj(R);
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

	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetSystemProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return bOk ? NA_SuccessStr(TEXT("System property set")) : FMonolithActionResult::Error(
		FString::Printf(TEXT("Unknown property '%s'. Supported: WarmupTime, WarmupTickCount, WarmupTickDelta, bFixedTickDelta, FixedTickDeltaTime, bDeterminism, RandomSeed, MaxPoolSize, or any UNiagaraSystem UProperty name."), *PropertyName));
}

// ============================================================================
// Action: set_static_switch_value
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSetStaticSwitchValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	UEnum* SwitchEnum = TryGetStaticSwitchEnum(SwitchPin, MN);

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

	FString DisplayValue = ValStr;
	if (SwitchEnum)
	{
		FString ResolvedRawValue;
		FString ResolvedDisplayValue;
		if (!ResolveStaticSwitchEnumValue(SwitchEnum, ValStr, ResolvedRawValue, &ResolvedDisplayValue))
		{
			TArray<FString> ValidOptions;
			for (int32 Index = 0; Index < SwitchEnum->NumEnums(); ++Index)
			{
				const int64 OptionValue = SwitchEnum->GetValueByIndex(Index);
				if (!SwitchEnum->IsValidEnumValue(OptionValue)) continue;
				ValidOptions.Add(FString::Printf(TEXT("%s (%s)"),
					*SwitchEnum->GetDisplayNameTextByIndex(Index).ToString(),
					*SwitchEnum->GetNameStringByValue(OptionValue)));
			}

			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Enum switch '%s' does not accept value '%s'. Valid options: [%s]"),
				*InputName,
				*ValStr,
				*FString::Join(ValidOptions, TEXT(", "))));
		}

		ValStr = ResolvedRawValue;
		DisplayValue = ResolvedDisplayValue.IsEmpty() ? ResolvedRawValue : ResolvedDisplayValue;
	}

	SwitchPin->DefaultValue = ValStr;

	GEditor->EndTransaction();
	System->RequestCompile(false);

	return NA_SuccessStr(FString::Printf(TEXT("Static switch '%s' set to '%s' (raw='%s')"), *InputName, *DisplayValue, *ValStr));
}

// ============================================================================
// Wave 2: Summary & Discovery Actions (4 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetSystemSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));
	EMonolithSemanticDetailLevel DetailLevel = EMonolithSemanticDetailLevel::Compact;
	FString DetailError;
	if (!TryParseSemanticDetailLevel(Params, DetailLevel, DetailError))
	{
		return FMonolithActionResult::Error(DetailError);
	}
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TArray<FMonolithNiagaraTopologyEdge> TopologyEdges;
	CollectTopologyEdges(System, TopologyEdges);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("system_name"), System->GetName());
	R->SetStringField(TEXT("detail_level"), DetailLevel == EMonolithSemanticDetailLevel::Full ? TEXT("full") : TEXT("compact"));

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
	TArray<TSharedPtr<FJsonValue>> EmitterArr;
	TArray<TSharedPtr<FJsonValue>> TopologyArr;
	TArray<TSharedPtr<FJsonValue>> IndependentBurstArr;
	int32 TotalModuleCount = 0;
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		const FMonolithNiagaraEmitterSemantic Semantic = AnalyzeEmitterSemantic(System, Handle, i, TopologyEdges);
		TSharedRef<FJsonObject> EObj = MakeShared<FJsonObject>();
		EObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EObj->SetNumberField(TEXT("index"), i);
		EObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EObj->SetStringField(TEXT("sim_target"), ED && ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));
		if (ED) EObj->SetBoolField(TEXT("local_space"), ED->bLocalSpace != 0);
		AppendEmitterSemanticJson(EObj, Semantic, DetailLevel);

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

		if (Semantic.RoleHint == TEXT("independent_burst"))
		{
			TSharedRef<FJsonObject> BurstObj = MakeShared<FJsonObject>();
			BurstObj->SetStringField(TEXT("emitter"), Semantic.Name);
			BurstObj->SetStringField(TEXT("reason"), TEXT("Burst-like emitter has no incoming event handler and will behave independently from other emitters."));
			IndependentBurstArr.Add(MakeShared<FJsonValueObject>(BurstObj));
		}
	}

	if (DetailLevel == EMonolithSemanticDetailLevel::Full)
	{
		for (const FMonolithNiagaraTopologyEdge& Edge : TopologyEdges)
		{
			TopologyArr.Add(MakeShared<FJsonValueObject>(MakeTopologyEdgeJson(Edge, DetailLevel)));
		}
	}

	R->SetArrayField(TEXT("emitters"), EmitterArr);
	R->SetNumberField(TEXT("inter_emitter_link_count"), TopologyEdges.Num());
	if (DetailLevel == EMonolithSemanticDetailLevel::Full)
	{
		R->SetArrayField(TEXT("inter_emitter_topology"), TopologyArr);
	}
	R->SetArrayField(TEXT("independent_burst_emitters"), IndependentBurstArr);
	R->SetNumberField(TEXT("emitter_count"), Handles.Num());
	R->SetNumberField(TEXT("total_module_count"), TotalModuleCount);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetEmitterSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));
	EMonolithSemanticDetailLevel DetailLevel = EMonolithSemanticDetailLevel::Compact;
	FString DetailError;
	if (!TryParseSemanticDetailLevel(Params, DetailLevel, DetailError))
	{
		return FMonolithActionResult::Error(DetailError);
	}

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));
	TArray<FMonolithNiagaraTopologyEdge> TopologyEdges;
	CollectTopologyEdges(System, TopologyEdges);
	const FMonolithNiagaraEmitterSemantic Semantic = AnalyzeEmitterSemantic(System, Handle, EIdx, TopologyEdges);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("name"), Handle.GetName().ToString());
	R->SetStringField(TEXT("detail_level"), DetailLevel == EMonolithSemanticDetailLevel::Full ? TEXT("full") : TEXT("compact"));
	R->SetStringField(TEXT("sim_target"), ED->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPU") : TEXT("CPU"));
	R->SetBoolField(TEXT("local_space"), ED->bLocalSpace != 0);
	R->SetBoolField(TEXT("determinism"), ED->bDeterminism != 0);
	AppendEmitterSemanticJson(R, Semantic, DetailLevel);

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

	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleListEmitterProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
			return NA_SuccessObj(R);
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
		return NA_SuccessObj(R);
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

	return NA_SuccessObj(R);
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
	FString SystemPath = NA_GetAssetPath(Params);
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
	if (!InputType.IsDataInterface()) return FMonolithActionResult::Error(FString::Printf(
		TEXT("Input '%s' is a plain value type (%s), not a DataInterface curve. Use set_curve_value to animate it: "
			 "{\"op\": \"set_curve_value\", \"input\": \"%s\", \"keys\": [{\"time\": 0, \"value\": 0}, ...]}"),
		*InputName, *InputType.GetName(), *InputName));

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
		// Plain {time, value} format — normalize keys first, then check DI type.
		// Bug fix: if the DI is a ColorCurve, plain float keys used to build a Config with only
		// a "keys" field, which ApplyCurveConfig never reads for ColorCurve (it looks for
		// "red"/"green"/"blue"/"alpha"). The fix: when the target DI is a ColorCurve, fan the
		// scalar value out to all four RGBA channels so the caller doesn't have to.
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

		if (Cast<UNiagaraDataInterfaceColorCurve>(DI))
		{
			// ColorCurve DI but plain scalar keys provided — fan the value out to all RGBA
			// channels. This is the most common intent: "fade all channels by the same curve".
			// Callers who want per-channel control should use r/g/b/a key fields instead.
			TSharedRef<FJsonValueArray> ChannelKeys = MakeShared<FJsonValueArray>(FloatKeys);
			Config->SetField(TEXT("red"),   ChannelKeys);
			Config->SetField(TEXT("green"), ChannelKeys);
			Config->SetField(TEXT("blue"),  ChannelKeys);
			Config->SetField(TEXT("alpha"), ChannelKeys);
		}
		else
		{
			Config->SetField(TEXT("keys"), MakeShared<FJsonValueArray>(FloatKeys));
		}
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
	return NA_SuccessObj(Result);
}

FMonolithActionResult FMonolithNiagaraActions::HandleConfigureDataInterface(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	TArray<TSharedPtr<FJsonValue>> PropsFailed;
	for (auto& Pair : Properties->Values)
	{
		FProperty* Prop = DI->GetClass()->FindPropertyByName(FName(*MonolithKeyToString(Pair.Key)));
		if (!Prop) { PropsNotFound.Add(MonolithKeyToString(Pair.Key)); continue; }
		void* Addr = Prop->ContainerPtrToValuePtr<void>(DI);

		// Build value string — handle JSON arrays → UE array syntax, JSON objects → UE struct syntax
		FString ValStr;
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			if (Pair.Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> JsonArr = Pair.Value->AsArray();
				bool bQuoteElements = CastField<FNameProperty>(ArrayProp->Inner)
					|| CastField<FStrProperty>(ArrayProp->Inner);
				ValStr = TEXT("(");
				for (int32 ArrIdx = 0; ArrIdx < JsonArr.Num(); ++ArrIdx)
				{
					if (ArrIdx > 0) ValStr += TEXT(",");
					if (bQuoteElements)
						ValStr += TEXT("\"") + JsonArr[ArrIdx]->AsString() + TEXT("\"");
					else
						ValStr += JsonArr[ArrIdx]->AsString();
				}
				ValStr += TEXT(")");
			}
			else { ValStr = Pair.Value->AsString(); }
		}
		else if (CastField<FStructProperty>(Prop))
		{
			if (Pair.Value->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Obj = Pair.Value->AsObject();
				ValStr = TEXT("(");
				bool bFirst = true;
				for (auto& KV : Obj->Values)
				{
					if (!bFirst) ValStr += TEXT(",");
					bFirst = false;
					ValStr += MonolithKeyToString(KV.Key) + TEXT("=") + KV.Value->AsString();
				}
				ValStr += TEXT(")");
			}
			else { ValStr = Pair.Value->AsString(); }
		}
		else if (Pair.Value->Type == EJson::Number) { ValStr = FString::SanitizeFloat(Pair.Value->AsNumber()); }
		else if (Pair.Value->Type == EJson::Boolean) { ValStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); }
		else { ValStr = Pair.Value->AsString(); }

		if (Prop->ImportText_Direct(*ValStr, Addr, DI, PPF_None))
		{
			PropsSet.Add(MonolithKeyToString(Pair.Key));
		}
		else
		{
			TSharedRef<FJsonObject> FailEntry = MakeShared<FJsonObject>();
			FailEntry->SetStringField(TEXT("property"), MonolithKeyToString(Pair.Key));
			FailEntry->SetStringField(TEXT("value"), ValStr);
			FailEntry->SetStringField(TEXT("error"), FString::Printf(TEXT("ImportText_Direct failed for property '%s' with value '%s'"), *MonolithKeyToString(Pair.Key), *ValStr));
			PropsFailed.Add(MakeShared<FJsonValueObject>(FailEntry));
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

	// Build available_properties list (CPF_Edit properties on the DI)
	TArray<TSharedPtr<FJsonValue>> AvailableProps;
	for (TFieldIterator<FProperty> It(DI->GetClass()); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Edit))
			AvailableProps.Add(MakeShared<FJsonValueString>(It->GetName()));
	}

	// Return error if nothing was set (all properties were either not found or failed import)
	if (PropsSet.IsEmpty() && (!PropsNotFound.IsEmpty() || !PropsFailed.IsEmpty()))
	{
		TArray<FString> AvailNames;
		for (const auto& V : AvailableProps) AvailNames.Add(V->AsString());
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No properties were set on DI '%s'. Not found: [%s]. Available: [%s]"),
			*DI->GetClass()->GetName(),
			*FString::Join(PropsNotFound, TEXT(", ")),
			*FString::Join(AvailNames, TEXT(", "))));
	}

	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("di_class"), DI->GetClass()->GetName());
	TArray<TSharedPtr<FJsonValue>> SetArr;
	for (const FString& S : PropsSet) SetArr.Add(MakeShared<FJsonValueString>(S));
	R->SetArrayField(TEXT("properties_set"), SetArr);
	if (!PropsNotFound.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& S : PropsNotFound) NotFoundArr.Add(MakeShared<FJsonValueString>(S));
		R->SetArrayField(TEXT("properties_not_found"), NotFoundArr);
		R->SetArrayField(TEXT("available_properties"), AvailableProps);
	}
	if (!PropsFailed.IsEmpty())
	{
		R->SetArrayField(TEXT("properties_failed"), PropsFailed);
		if (PropsNotFound.IsEmpty()) R->SetArrayField(TEXT("available_properties"), AvailableProps);
	}
	return NA_SuccessObj(R);
}

// ============================================================================
// Wave 4: System Management Actions (5 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleDuplicateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: save_path"));

	// Validate the destination before loading the source system or calling AssetTools.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load source system"));

	// Parse save_path into package path + asset name
	const FString DestPath = FPackageName::GetLongPackagePath(SavePath);
	const FString NewName = FPackageName::GetLongPackageAssetName(SavePath);

	FAssetToolsModule& ATModule = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* Dup = ATModule.Get().DuplicateAsset(NewName, DestPath, System);
	UNiagaraSystem* DupSystem = Cast<UNiagaraSystem>(Dup);
	if (!DupSystem) return FMonolithActionResult::Error(TEXT("DuplicateAsset failed"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("asset_path"), DupSystem->GetPathName());
	R->SetNumberField(TEXT("emitter_count"), DupSystem->GetEmitterHandles().Num());
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetFixedBounds(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	TOptional<FVector> MinV;
	TOptional<FVector> MaxV;
	if (bEnabled)
	{
		MinV = ParseVec3(Params, TEXT("min"));
		MaxV = ParseVec3(Params, TEXT("max"));
		if (!MinV.IsSet() || !MaxV.IsSet())
			return FMonolithActionResult::Error(TEXT("Both 'min' and 'max' are required as [x,y,z] arrays when enabling bounds"));
	}

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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEffectType(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleCreateEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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
	FString GpuWarning;
	if (SimTarget == TEXT("gpu") && !EmitterId.IsEmpty())
	{
		TSharedRef<FJsonObject> SP = MakeShared<FJsonObject>();
		SP->SetStringField(TEXT("asset_path"), SystemPath);
		SP->SetStringField(TEXT("emitter"), EmitterId);
		SP->SetStringField(TEXT("property"), TEXT("SimTarget"));
		SP->SetStringField(TEXT("value"), TEXT("GPUComputeSim"));
		FMonolithActionResult PropResult = HandleSetEmitterProperty(SP);
		if (!PropResult.bSuccess)
		{
			// GPU target set failed — report but don't fail the whole creation
			GpuWarning = FString::Printf(TEXT("Emitter created but sim target set failed: %s"), *PropResult.ErrorMessage);
		}
	}

	int32 EIdx = FindEmitterHandleIndex(System, EmitterId);
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("emitter_name"), EmitterName);
	R->SetNumberField(TEXT("emitter_index"), EIdx);
	R->SetStringField(TEXT("handle_id"), EmitterId);
	if (!GpuWarning.IsEmpty()) R->SetStringField(TEXT("gpu_warning"), GpuWarning);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleExportSystemSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	// System properties (expanded)
	TSharedRef<FJsonObject> SysProps = MakeShared<FJsonObject>();
	SysProps->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
	// bDeterminism via reflection (bitfield)
	FProperty* SysDetProp = System->GetClass()->FindPropertyByName(TEXT("bDeterminism"));
	if (SysDetProp)
	{
		const void* SysDetAddr = SysDetProp->ContainerPtrToValuePtr<void>(System);
		if (FBoolProperty* BP = CastField<FBoolProperty>(SysDetProp))
			SysProps->SetBoolField(TEXT("determinism"), BP->GetPropertyValue(SysDetAddr));
	}
	SysProps->SetBoolField(TEXT("fixed_bounds"), System->bFixedBounds != 0);
	if (System->bFixedBounds)
	{
		FBox SysBounds = System->GetFixedBounds();
		TSharedRef<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
		BoundsObj->SetStringField(TEXT("min"), FString::Printf(TEXT("(%f,%f,%f)"), SysBounds.Min.X, SysBounds.Min.Y, SysBounds.Min.Z));
		BoundsObj->SetStringField(TEXT("max"), FString::Printf(TEXT("(%f,%f,%f)"), SysBounds.Max.X, SysBounds.Max.Y, SysBounds.Max.Z));
		SysProps->SetObjectField(TEXT("bounds"), BoundsObj);
	}
	// RandomSeed, MaxPoolSize via reflection
	FProperty* SeedProp = System->GetClass()->FindPropertyByName(TEXT("RandomSeed"));
	if (SeedProp)
	{
		const void* SeedAddr = SeedProp->ContainerPtrToValuePtr<void>(System);
		if (FIntProperty* IP = CastField<FIntProperty>(SeedProp))
			SysProps->SetNumberField(TEXT("random_seed"), IP->GetPropertyValue(SeedAddr));
	}
	FProperty* PoolProp = System->GetClass()->FindPropertyByName(TEXT("MaxPoolSize"));
	if (PoolProp)
	{
		const void* PoolAddr = PoolProp->ContainerPtrToValuePtr<void>(System);
		if (FIntProperty* IP = CastField<FIntProperty>(PoolProp))
			SysProps->SetNumberField(TEXT("max_pool_size"), IP->GetPropertyValue(PoolAddr));
		else if (FUInt32Property* UP32 = CastField<FUInt32Property>(PoolProp))
			SysProps->SetNumberField(TEXT("max_pool_size"), UP32->GetPropertyValue(PoolAddr));
	}
	UNiagaraEffectType* SysET = System->GetEffectType();
	if (SysET) SysProps->SetStringField(TEXT("effect_type"), SysET->GetPathName());
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
		if (UNiagaraEmitter* EmitterObj = Handle.GetInstance().Emitter)
		{
			EO->SetStringField(TEXT("asset"), EmitterObj->GetPathName());
		}

		// Emitter properties
		EO->SetBoolField(TEXT("local_space"), ED->bLocalSpace != 0);
		EO->SetBoolField(TEXT("determinism"), ED->bDeterminism != 0);
		{
			FString BoundsMode;
			switch (ED->CalculateBoundsMode)
			{
			case ENiagaraEmitterCalculateBoundMode::Dynamic: BoundsMode = TEXT("Dynamic"); break;
			case ENiagaraEmitterCalculateBoundMode::Fixed: BoundsMode = TEXT("Fixed"); break;
			case ENiagaraEmitterCalculateBoundMode::Programmable: BoundsMode = TEXT("Programmable"); break;
			default: BoundsMode = TEXT("Unknown"); break;
			}
			EO->SetStringField(TEXT("calculate_bounds_mode"), BoundsMode);
		}
		{
			FString AllocMode;
			switch (ED->AllocationMode)
			{
			case EParticleAllocationMode::AutomaticEstimate: AllocMode = TEXT("AutomaticEstimate"); break;
			case EParticleAllocationMode::ManualEstimate: AllocMode = TEXT("ManualEstimate"); break;
			case EParticleAllocationMode::FixedCount: AllocMode = TEXT("FixedCount"); break;
			default: AllocMode = TEXT("Unknown"); break;
			}
			EO->SetStringField(TEXT("allocation_mode"), AllocMode);
		}
		EO->SetNumberField(TEXT("pre_allocation_count"), ED->PreAllocationCount);

		// Event handlers
		{
			const TArray<FNiagaraEventScriptProperties>& Handlers = ED->GetEventHandlers();
			if (Handlers.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> EHArr;
				for (const FNiagaraEventScriptProperties& ESP : Handlers)
				{
					TSharedRef<FJsonObject> EHO = MakeShared<FJsonObject>();
					EHO->SetStringField(TEXT("source_event"), ESP.SourceEventName.ToString());
					EHO->SetStringField(TEXT("source_emitter_id"), ESP.SourceEmitterID.ToString());
					// Resolve source emitter name
					if (ESP.SourceEmitterID.IsValid())
					{
						for (const FNiagaraEmitterHandle& SrcH : System->GetEmitterHandles())
						{
							if (SrcH.GetId() == ESP.SourceEmitterID)
							{
								EHO->SetStringField(TEXT("source_emitter_name"), SrcH.GetName().ToString());
								break;
							}
						}
					}
					FString ExecModeStr;
					switch (ESP.ExecutionMode)
					{
					case EScriptExecutionMode::EveryParticle: ExecModeStr = TEXT("EveryParticle"); break;
					case EScriptExecutionMode::SpawnedParticles: ExecModeStr = TEXT("SpawnedParticles"); break;
					case EScriptExecutionMode::SingleParticle: ExecModeStr = TEXT("SingleParticle"); break;
					default: ExecModeStr = TEXT("Unknown"); break;
					}
					EHO->SetStringField(TEXT("execution_mode"), ExecModeStr);
					EHO->SetNumberField(TEXT("spawn_number"), static_cast<double>(ESP.SpawnNumber));
					EHO->SetNumberField(TEXT("max_events"), static_cast<double>(ESP.MaxEventsPerFrame));
					if (ESP.Script) EHO->SetStringField(TEXT("usage_id"), ESP.Script->GetUsageId().ToString());
					EHArr.Add(MakeShared<FJsonValueObject>(EHO));
				}
				EO->SetArrayField(TEXT("event_handlers"), EHArr);
			}
		}

		// Simulation stages
		{
			const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();
			if (Stages.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> SSArr;
				for (UNiagaraSimulationStageBase* StageBase : Stages)
				{
					if (!StageBase) continue;
					TSharedRef<FJsonObject> SSO = MakeShared<FJsonObject>();
					SSO->SetStringField(TEXT("name"), StageBase->SimulationStageName.ToString());
					if (StageBase->Script) SSO->SetStringField(TEXT("usage_id"), StageBase->Script->GetUsageId().ToString());
					if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(StageBase))
					{
						FString IterStr;
						switch (Generic->IterationSource)
						{
						case ENiagaraIterationSource::Particles: IterStr = TEXT("Particles"); break;
						case ENiagaraIterationSource::DataInterface: IterStr = TEXT("DataInterface"); break;
						case ENiagaraIterationSource::DirectSet: IterStr = TEXT("DirectSet"); break;
						default: IterStr = TEXT("Unknown"); break;
						}
						SSO->SetStringField(TEXT("iteration_source"), IterStr);
					}
					SSArr.Add(MakeShared<FJsonValueObject>(SSO));
				}
				EO->SetArrayField(TEXT("simulation_stages"), SSArr);
			}
		}

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

			int32 ExportEmitterIdx = FindEmitterHandleIndex(System, HandleId);

			for (UNiagaraNodeFunctionCall* MN : Mods)
			{
				if (!MN) continue;
				TSharedRef<FJsonObject> MO = MakeShared<FJsonObject>();
				MO->SetStringField(TEXT("stage"), StageName);
				MO->SetStringField(TEXT("guid"), MN->NodeGuid.ToString());
				FString ScriptPath;
				if (MN->FunctionScript)
					ScriptPath = MN->FunctionScript->GetPathName();
				MO->SetStringField(TEXT("script"), ScriptPath);

				if (bIncludeValues)
				{
					TSharedRef<FJsonObject> InputsObj = MakeShared<FJsonObject>();
					// Get override pin values
					TArray<FNiagaraVariable> ModInputs;
					if (ExportEmitterIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[ExportEmitterIdx].GetInstance();
						FCompileConstantResolver Resolver(VE, Usage);
						FNiagaraStackGraphUtilities::GetStackFunctionInputs(*MN, ModInputs, Resolver,
							FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
					}

					// Dynamic inputs array
					TArray<TSharedPtr<FJsonValue>> DynInputsArr;

					for (const FNiagaraVariable& In : ModInputs)
					{
						FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
							FNiagaraParameterHandle(In.GetName()), MN);
						UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*MN, AH);
						if (OP && OP->LinkedTo.Num() > 0)
						{
							// Check if linked to a dynamic input (UNiagaraNodeFunctionCall)
							UEdGraphPin* LinkedPin = OP->LinkedTo[0];
							if (LinkedPin && LinkedPin->GetOwningNode())
							{
								UNiagaraNodeFunctionCall* DynNode = Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNode());
								if (DynNode && DynNode != MN)
								{
									TSharedRef<FJsonObject> DynObj = MakeShared<FJsonObject>();
									FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
									DynObj->SetStringField(TEXT("input"), ShortName.ToString());
									DynObj->SetStringField(TEXT("name"), DynNode->GetFunctionName());
									if (DynNode->FunctionScript)
										DynObj->SetStringField(TEXT("script_path"), DynNode->FunctionScript->GetPathName());
									DynObj->SetStringField(TEXT("guid"), DynNode->NodeGuid.ToString());
									DynInputsArr.Add(MakeShared<FJsonValueObject>(DynObj));
								}
							}
						}
						else if (OP && OP->LinkedTo.Num() == 0 && !OP->DefaultValue.IsEmpty())
						{
							FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());
							InputsObj->SetStringField(ShortName.ToString(), OP->DefaultValue);
						}
					}
					if (InputsObj->Values.Num() > 0)
						MO->SetObjectField(TEXT("inputs"), InputsObj);
					if (DynInputsArr.Num() > 0)
						MO->SetArrayField(TEXT("dynamic_inputs"), DynInputsArr);

					// Static switches
					TArray<UEdGraphPin*> StaticSwitchPins;
					TSet<UEdGraphPin*> HiddenSwitchPins;
					if (ExportEmitterIdx != INDEX_NONE)
					{
						FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[ExportEmitterIdx].GetInstance();
						FCompileConstantResolver SwitchResolver(VE, Usage);
						FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
							*MN, StaticSwitchPins, HiddenSwitchPins, SwitchResolver);
					}
					if (StaticSwitchPins.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> SwitchArr;
						for (UEdGraphPin* SwitchPin : StaticSwitchPins)
						{
							if (!SwitchPin) continue;
							TSharedRef<FJsonObject> SwObj = MakeShared<FJsonObject>();
							SwObj->SetStringField(TEXT("name"), SwitchPin->GetFName().ToString());
							SwObj->SetStringField(TEXT("value"), SwitchPin->DefaultValue);
							SwitchArr.Add(MakeShared<FJsonValueObject>(SwObj));
						}
						MO->SetArrayField(TEXT("static_switches"), SwitchArr);
					}
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
	return NA_SuccessObj(R);
}

// ============================================================================
// Wave 5: Dynamic Input Actions (3 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddDynamicInput(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	if (!OutDynNode)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("SetDynamicInputForFunctionInput returned null node"));
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	// Read back the dynamic input's own inputs
	// Use GetStackFunctionInputs to capture all inputs (static switches AND data inputs)
	TArray<TSharedPtr<FJsonValue>> DynInputsArr;
	TArray<FNiagaraVariable> DynNodeInputs;
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE2 = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver DynResolver(VE2, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*OutDynNode, DynNodeInputs, DynResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver DynResolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*OutDynNode, DynNodeInputs, DynResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	for (const FNiagaraVariable& In : DynNodeInputs)
	{
		TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
		IO->SetStringField(TEXT("name"), MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString());
		IO->SetStringField(TEXT("type"), In.GetType().GetName());
		// Try to get the current default value from the override pin
		FNiagaraParameterHandle DynAH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(In.GetName()), OutDynNode);
		UEdGraphPin* DynOP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*OutDynNode, DynAH);
		if (DynOP && DynOP->LinkedTo.Num() == 0)
			IO->SetStringField(TEXT("default"), DynOP->DefaultValue);
		DynInputsArr.Add(MakeShared<FJsonValueObject>(IO));
	}
	// Fall back to direct pin iteration if GetStackFunctionInputs returned nothing
	if (DynInputsArr.IsEmpty())
	{
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
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("dynamic_input_node_guid"), OutDynNode->NodeGuid.ToString());
	R->SetStringField(TEXT("dynamic_input_name"), OutDynNode->GetFunctionName());
	R->SetArrayField(TEXT("inputs"), DynInputsArr);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetDynamicInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

		// Type filter — exclude unknowns and non-matching types when filter is specified
		if (!InputType.IsEmpty() && (InferredType == TEXT("unknown") || !InferredType.Equals(InputType, ESearchCase::IgnoreCase)))
			continue;

		Results.Add(MakeShared<FJsonValueObject>(Entry));
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetArrayField(TEXT("dynamic_inputs"), Results);
	return NA_SuccessObj(R);
}

// ============================================================================
// Wave 6: Advanced Actions (3 new)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleAddEventHandler(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
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

	if (SourceEmitterStr.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_emitter is required for add_event_handler. Self-events are not created implicitly; pass source_emitter explicitly so the handler does not end up unresolved."));
	}

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

	// Create a proper event script — direct array push leaves Script=nullptr
	// which crashes during RequestCompile.
	FNiagaraEmitterHandle& EmitterHandle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitter VersionedEmitter = EmitterHandle.GetInstance();
	UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
	if (!Emitter)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to get emitter instance"));
	}

	Handler.Script = NewObject<UNiagaraScript>(Emitter, MakeUniqueObjectName(Emitter, UNiagaraScript::StaticClass(), TEXT("EventScript")), RF_Transactional);
	Handler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	Handler.Script->SetUsageId(FGuid::NewGuid());
	if (ED->GraphSource)
	{
		Handler.Script->SetLatestSource(ED->GraphSource);
	}

	Emitter->AddEventHandler(Handler, VersionedEmitter.Version);
	if (ED->GraphSource)
	{
		if (UNiagaraScriptSource* GraphSource = Cast<UNiagaraScriptSource>(ED->GraphSource))
		{
			if (GraphSource->NodeGraph)
			{
				MonolithNiagaraHelpers::ResetGraphForOutputLocal(*GraphSource->NodeGraph,
					ENiagaraScriptUsage::ParticleEventScript,
					Handler.Script->GetUsageId());
			}
		}
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("message"), FString::Printf(TEXT("Added event handler for '%s'. Receive<Event> modules are not added automatically; if this handler must consume payloads such as Position/Velocity/Color, add the matching Receive module to the particle_event script and set required payload fields to Apply."), *EventName));
	R->SetNumberField(TEXT("handler_index"), ED->EventHandlerScriptProps.Num() - 1);
	R->SetStringField(TEXT("usage_id"), Handler.Script->GetUsageId().ToString());
	R->SetStringField(TEXT("usage"), TEXT("particle_event"));
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleValidateSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TArray<FMonolithNiagaraTopologyEdge> TopologyEdges;
	CollectTopologyEdges(System, TopologyEdges);

	// O(N) per-emitter semantic cache, keyed by emitter handle GUID (same key already used
	// by AppendEmitterSemanticJson::guid and FMonolithNiagaraTopologyEdge::SourceEmitterId,
	// so the inner event-chain lookup matches the serialised payload exactly). Replaces an
	// O(N^2) re-analysis path that called AnalyzeEmitterSemantic once per emitter in the
	// outer loop AND once per source-emitter in the inner event-chain loop.
	TMap<FGuid, FMonolithNiagaraEmitterSemantic> SemanticCache;
	SemanticCache.Reserve(Handles.Num());
	for (int32 CacheIdx = 0; CacheIdx < Handles.Num(); ++CacheIdx)
	{
		const FNiagaraEmitterHandle& CacheHandle = Handles[CacheIdx];
		SemanticCache.Add(CacheHandle.GetId(), AnalyzeEmitterSemantic(System, CacheHandle, CacheIdx, TopologyEdges));
	}

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

	for (const FMonolithNiagaraTopologyEdge& Edge : TopologyEdges)
	{
		if (!Edge.bSourceEmitterResolved)
		{
			Warnings.Add(MakeEntry(Edge.TargetEmitterName,
				FString::Printf(TEXT("Consumes %s but the source emitter could not be resolved from SourceEmitterID '%s'"),
					*Edge.EventName,
					*Edge.SourceEmitterId)));
		}
	}

	// Per-emitter checks
	for (int32 EmitterIndex = 0; EmitterIndex < Handles.Num(); ++EmitterIndex)
	{
		const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;
		FString EN = Handle.GetName().ToString();
		const FMonolithNiagaraEmitterSemantic& Semantic = SemanticCache.FindChecked(Handle.GetId());

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
			else if (UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(Rend))
			{
				if (Mesh->Meshes.Num() == 0)
				{
					Warnings.Add(MakeEntry(EN, TEXT("Mesh renderer has no meshes assigned")));
				}
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

		// --- Phase 8 deep validation checks ---

		// Collect module script paths per stage for structural checks
		FString HandleId = Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);

		auto CollectModuleScriptPaths = [&](ENiagaraScriptUsage Usage) -> TArray<FString>
		{
			TArray<FString> Paths;
			UNiagaraNodeOutput* OutNode = FindOutputNode(System, HandleId, Usage);
			if (OutNode)
			{
				TArray<UNiagaraNodeFunctionCall*> Modules;
				MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutNode, Modules);
				for (UNiagaraNodeFunctionCall* MN : Modules)
				{
					if (MN && MN->FunctionScript)
						Paths.Add(MN->FunctionScript->GetPathName());
				}
			}
			return Paths;
		};

		TArray<FString> EmitterUpdatePaths = CollectModuleScriptPaths(ENiagaraScriptUsage::EmitterUpdateScript);
		TArray<FString> EmitterSpawnPaths = CollectModuleScriptPaths(ENiagaraScriptUsage::EmitterSpawnScript);
		TArray<FString> ParticleSpawnPaths = CollectModuleScriptPaths(ENiagaraScriptUsage::ParticleSpawnScript);
		TArray<FString> ParticleUpdatePaths = CollectModuleScriptPaths(ENiagaraScriptUsage::ParticleUpdateScript);

		// Check 1: No spawn module
		{
			bool bHasSpawn = false;
			auto ContainsSpawn = [](const TArray<FString>& Paths) -> bool
			{
				for (const FString& P : Paths)
				{
					if (P.Contains(TEXT("Spawn"))) return true;
				}
				return false;
			};
			bHasSpawn = ContainsSpawn(EmitterUpdatePaths) || ContainsSpawn(EmitterSpawnPaths);
			if (!bHasSpawn)
			{
				Warnings.Add(MakeEntry(EN, TEXT("No spawn module found — emitter will produce no particles")));
			}
		}

		// Check 2: Missing SolveForcesAndVelocity
		{
			bool bHasForce = false;
			bool bHasSolve = false;
			int32 SolveIndex = INDEX_NONE;
			int32 FirstLateForceIndex = INDEX_NONE;
			FString FirstLateForceName;
			for (int32 ModuleIndex = 0; ModuleIndex < ParticleUpdatePaths.Num(); ++ModuleIndex)
			{
				const FString& P = ParticleUpdatePaths[ModuleIndex];
				if (P.Contains(TEXT("Force")) || P.Contains(TEXT("Gravity")) || P.Contains(TEXT("Drag")) || P.Contains(TEXT("Wind")))
				{
					bHasForce = true;
					if (SolveIndex != INDEX_NONE && FirstLateForceIndex == INDEX_NONE)
					{
						FirstLateForceIndex = ModuleIndex;
						FirstLateForceName = FPaths::GetBaseFilename(P);
					}
				}
				if (P.Contains(TEXT("SolveForcesAndVelocity")))
				{
					bHasSolve = true;
					SolveIndex = ModuleIndex;
				}
			}
			if (bHasForce && !bHasSolve)
			{
				Warnings.Add(MakeEntry(EN, TEXT("Force/Gravity/Drag/Wind module present but SolveForcesAndVelocity missing — forces will have no effect")));
			}
			else if (SolveIndex != INDEX_NONE && FirstLateForceIndex != INDEX_NONE)
			{
				Warnings.Add(MakeEntry(EN, FString::Printf(
					TEXT("Particle Update module order is invalid: SolveForcesAndVelocity appears before '%s'. Force/Gravity/Drag/Wind modules should come before SolveForcesAndVelocity so their effects are integrated."),
					*FirstLateForceName)));
			}
		}

		// Check 2b: GenerateDeathEvent requires persistent IDs
		{
			bool bHasGenerateDeathEvent = false;
			for (const FString& P : ParticleUpdatePaths)
			{
				if (P.Contains(TEXT("GenerateDeathEvent")))
				{
					bHasGenerateDeathEvent = true;
					break;
				}
			}

			if (bHasGenerateDeathEvent && !ED->bRequiresPersistentIDs)
			{
				Errors.Add(MakeEntry(EN, TEXT("GenerateDeathEvent is present in Particle Update but requires_persistent_ids is false. Enable requires_persistent_ids on the emitter before relying on death-event chaining.")));
			}
		}

		// Check 3: Missing InitializeParticle
		{
			bool bHasInit = false;
			for (const FString& P : ParticleSpawnPaths)
			{
				if (P.Contains(TEXT("InitializeParticle")))
				{
					bHasInit = true;
					break;
				}
			}
			if (!bHasInit)
			{
				Suggestions.Add(MakeEntry(EN, TEXT("No InitializeParticle module — particles may have uninitialized attributes")));
			}
		}

		// Check 3b: renderer-required attributes left effectively uninitialized while later modules consume them.
		{
			static const MonolithNiagaraHelpers::FNA_RendererAttributeHazardRule HazardRules[] = {
				{ TEXT("NiagaraSpriteRendererProperties"), TEXT("Particles.SpriteSize"), TEXT("SpriteSize"), TEXT("ScaleSpriteSize") },
				{ TEXT("NiagaraRibbonRendererProperties"), TEXT("Particles.RibbonWidth"), TEXT("RibbonWidth"), TEXT("Ribbon") },
				{ TEXT("NiagaraMeshRendererProperties"), TEXT("Particles.MeshScale"), TEXT("MeshScale"), TEXT("ScaleMesh") },
			};

			for (const MonolithNiagaraHelpers::FNA_RendererAttributeHazardRule& Rule : HazardRules)
			{
				if (!MonolithNiagaraHelpers::NA_HasRendererClass(ED->GetRenderers(), Rule.RendererClassName))
				{
					continue;
				}

				const bool bHasConsumer = MonolithNiagaraHelpers::NA_HasConsumerModule(ParticleSpawnPaths, Rule.ConsumerToken)
					|| MonolithNiagaraHelpers::NA_HasConsumerModule(ParticleUpdatePaths, Rule.ConsumerToken);
				if (!bHasConsumer)
				{
					continue;
				}

				const bool bHasAttributeInit = MonolithNiagaraHelpers::NA_HasAttributeInitializationModule(ParticleSpawnPaths, Rule.FriendlyAttributeName);
				if (!bHasAttributeInit)
				{
					Warnings.Add(MakeEntry(EN,
						FString::Printf(TEXT("Renderer depends on %s, but no particle-spawn initialization for %s was detected before later modules consume it. The emitter may compile successfully but render nothing."),
							Rule.AttributeName,
							Rule.FriendlyAttributeName)));
				}
			}
		}

		// Check 4: Material usage flag mismatch
		for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
		{
			if (!Rend || !Rend->GetIsEnabled()) continue;

			UMaterialInterface* MatIface = nullptr;
			EMaterialUsage RequiredUsage = MATUSAGE_MAX;

			if (UNiagaraSpriteRendererProperties* SR = Cast<UNiagaraSpriteRendererProperties>(Rend))
			{
				MatIface = SR->Material;
				RequiredUsage = MATUSAGE_NiagaraSprites;
			}
			else if (UNiagaraRibbonRendererProperties* RR = Cast<UNiagaraRibbonRendererProperties>(Rend))
			{
				MatIface = RR->Material;
				RequiredUsage = MATUSAGE_NiagaraRibbons;
			}
			else if (UNiagaraMeshRendererProperties* MR = Cast<UNiagaraMeshRendererProperties>(Rend))
			{
				if (MR->OverrideMaterials.Num() > 0 && MR->OverrideMaterials[0].ExplicitMat)
					MatIface = MR->OverrideMaterials[0].ExplicitMat;
				RequiredUsage = MATUSAGE_NiagaraMeshParticles;
			}

			if (MatIface && RequiredUsage != MATUSAGE_MAX)
			{
				UMaterial* BaseMat = MatIface->GetMaterial();
				if (BaseMat && !BaseMat->GetUsageByFlag(RequiredUsage))
				{
					Errors.Add(MakeEntry(EN, FString::Printf(
						TEXT("Material '%s' missing Niagara usage flag — will render default material at runtime"),
						*MatIface->GetName())));
				}

				// Check 5: DynamicParameter suggestion
				if (BaseMat)
				{
					bool bHasDynParam = false;
					for (UMaterialExpression* Expr : BaseMat->GetExpressions())
					{
						if (Cast<UMaterialExpressionDynamicParameter>(Expr))
						{
							bHasDynParam = true;
							break;
						}
					}
					if (bHasDynParam)
					{
						Suggestions.Add(MakeEntry(EN, FString::Printf(
							TEXT("Material '%s' uses DynamicParameter — verify emitter writes DynamicMaterialParameter attributes"),
							*MatIface->GetName())));
					}
				}
			}
		}

		// Event-chain semantics checks (cached source-emitter semantic lookup — see SemanticCache above)
		if (Semantic.IncomingEvents.Num() > 0)
		{
			for (const FMonolithNiagaraTopologyEdge& Incoming : Semantic.IncomingEvents)
			{
				FGuid HandlerUsageId;
				if (!FGuid::Parse(Incoming.UsageId, HandlerUsageId))
				{
					continue;
				}

				UNiagaraNodeOutput* EventOutput = FindOutputNode(System, Handle.GetId().ToString(), ENiagaraScriptUsage::ParticleEventScript, HandlerUsageId);
				if (!EventOutput)
				{
					Warnings.Add(MakeEntry(EN, FString::Printf(TEXT("Event handler for %s exists but its ParticleEventScript output node could not be resolved; the handler may be incomplete."), *Incoming.EventName)));
					continue;
				}

				TArray<UNiagaraNodeFunctionCall*> EventModules;
				MonolithNiagaraHelpers::GetOrderedModuleNodes(*EventOutput, EventModules);

				FString ReceiveSuffix = CanonicalizeEventName(Incoming.EventName);
				ReceiveSuffix.ReplaceInline(TEXT("Event"), TEXT(""), ESearchCase::CaseSensitive);
				const FString ExpectedReceiveToken = FString::Printf(TEXT("receive%s"), *ReceiveSuffix);
				bool bHasReceiveModule = false;
				for (UNiagaraNodeFunctionCall* EventModule : EventModules)
				{
					if (!EventModule) continue;
					const FString LowerName = EventModule->GetFunctionName().ToLower();
					if (LowerName.Contains(ExpectedReceiveToken.ToLower()))
					{
						bHasReceiveModule = true;
						break;
					}
				}

				if (!bHasReceiveModule)
				{
					const FString SuggestedReceiveModule = FString::Printf(TEXT("Receive%s"), *CanonicalizeEventName(Incoming.EventName));
					Warnings.Add(MakeEntry(EN,
						FString::Printf(TEXT("Event handler for %s exists, but its particle_event script has no matching Receive<Event> module. Add Receive%s and apply the payload fields you need."),
							*Incoming.EventName,
							*SuggestedReceiveModule)));
				}
			}

			for (const FMonolithNiagaraTopologyEdge& Incoming : Semantic.IncomingEvents)
			{
				bool bSourceGeneratesEvent = false;
				FGuid SourceGuid;
				if (FGuid::Parse(Incoming.SourceEmitterId, SourceGuid))
				{
					if (const FMonolithNiagaraEmitterSemantic* SourceSemantic = SemanticCache.Find(SourceGuid))
					{
						for (const FMonolithNiagaraEventGeneratorInfo& Generator : SourceSemantic->EventGenerators)
						{
							if (Generator.EventName == Incoming.EventName)
							{
								bSourceGeneratesEvent = true;
								break;
							}
						}
					}
				}

				if (!bSourceGeneratesEvent && Incoming.bSourceEmitterResolved)
				{
					Warnings.Add(MakeEntry(EN,
						FString::Printf(TEXT("Consumes %s from '%s', but the source emitter does not appear to generate that event"),
							*Incoming.EventName,
							*Incoming.SourceEmitterName)));
				}
			}

			if (Semantic.SpawnLocationMode == TEXT("mixed_event_and_local_shape"))
			{
				Warnings.Add(MakeEntry(EN, TEXT("Emitter consumes incoming events but also has local location modules; placement may still resolve to local origin/shape instead of purely following event payloads")));
			}

			if (!Semantic.bRequiresPersistentIDs)
			{
				Suggestions.Add(MakeEntry(EN, TEXT("Emitter participates in inter-emitter event flow while requires_persistent_ids is false; verify whether source-particle identity must remain stable across frames")));
			}
		}

		if (Semantic.RoleHint == TEXT("independent_burst"))
		{
			Suggestions.Add(MakeEntry(EN, TEXT("Burst-like emitter has no incoming event handler, so it will spawn independently instead of following another emitter's event location/death position")));
		}

		if (Semantic.RoleHint == TEXT("burst_receiver") && !Semantic.bRequiresPersistentIDs)
		{
			Suggestions.Add(MakeEntry(EN, TEXT("Burst receiver is event-driven but requires_persistent_ids is false; if burst timing/attachment should follow specific source particles, verify persistent ID requirements on the source chain")));
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	R->SetArrayField(TEXT("errors"), Errors);
	R->SetArrayField(TEXT("warnings"), Warnings);
	R->SetArrayField(TEXT("suggestions"), Suggestions);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system '%s'"), *SystemPath));

	FString EmitterName = Params->GetStringField(TEXT("emitter"));
	int32 HandleIdx = FindEmitterHandleIndex(System, EmitterName);
	if (HandleIdx == INDEX_NONE) return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found"), *EmitterName));

	FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[HandleIdx];
	FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
	UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
	if (!Emitter)
	{
		return FMonolithActionResult::Error(TEXT("Emitter instance is null"));
	}
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
	FString StageName;
	if (Params->TryGetStringField(TEXT("name"), StageName) && !StageName.IsEmpty())
	{
		NewStage->SimulationStageName = FName(*StageName);
	}

	FString IterSourceStr = Params->HasField(TEXT("iteration_source")) ? Params->GetStringField(TEXT("iteration_source")) : TEXT("particles");
	if (IterSourceStr.Equals(TEXT("data_interface"), ESearchCase::IgnoreCase))
	{
		NewStage->IterationSource = ENiagaraIterationSource::DataInterface;
	}

	// Add to emitter via exported API
	Emitter->AddSimulationStage(NewStage, VersionedEmitter.Version);
	if (Source && Source->NodeGraph)
	{
		MonolithNiagaraHelpers::ResetGraphForOutputLocal(*Source->NodeGraph,
			ENiagaraScriptUsage::ParticleSimulationStageScript,
			NewStage->Script->GetUsageId());
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);
	Emitter->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> GraphOutputs;
	if (Source && Source->NodeGraph)
	{
		for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
		{
			UNiagaraNodeOutput* OutNode = Cast<UNiagaraNodeOutput>(Node);
			if (!OutNode) continue;
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("usage"), UsageToString(OutNode->GetUsage()));
			O->SetStringField(TEXT("usage_id"), OutNode->GetUsageId().ToString());
			GraphOutputs.Add(MakeShared<FJsonValueObject>(O));
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("stage_name"), NewStage->SimulationStageName.ToString());
	R->SetStringField(TEXT("stage_id"), NewStage->GetMergeId().ToString());
	R->SetStringField(TEXT("usage_id"), NewStage->Script->GetUsageId().ToString());
	R->SetStringField(TEXT("iteration_source"), IterSourceStr);
	if (GraphOutputs.Num() > 0)
	{
		R->SetArrayField(TEXT("graph_outputs"), GraphOutputs);
	}
	return FMonolithActionResult::Success(R);
}

// ============================================================================
// Action: set_spawn_shape
// Composite: removes existing location modules, sets InitializeParticle position
// mode, adds the requested shape location module, and sets any shape parameters.
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSetSpawnShape(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	FString EmitterName = Params->GetStringField(TEXT("emitter"));
	FString Shape = Params->GetStringField(TEXT("shape"));
	bool bReplaceExisting = true;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

	if (AssetPath.IsEmpty() || EmitterName.IsEmpty() || Shape.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path, emitter, and shape are required"));

	Shape = Shape.ToLower();

	// Map shape name → module script path
	TMap<FString, FString> ShapeToModule;
	ShapeToModule.Add(TEXT("cylinder"), TEXT("/Niagara/Modules/Spawn/Location/CylinderLocation.CylinderLocation"));
	ShapeToModule.Add(TEXT("sphere"),   TEXT("/Niagara/Modules/Spawn/Location/SphereLocation.SphereLocation"));
	ShapeToModule.Add(TEXT("box"),      TEXT("/Niagara/Modules/Spawn/Location/BoxLocation.BoxLocation"));
	ShapeToModule.Add(TEXT("cone"),     TEXT("/Niagara/Modules/Spawn/Location/ConeLocation.ConeLocation"));
	ShapeToModule.Add(TEXT("torus"),    TEXT("/Niagara/Modules/Spawn/Location/TorusLocation.TorusLocation"));
	ShapeToModule.Add(TEXT("shape"),         TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("shapev2"),       TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("shapelocation"), TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	// Phase 10: extended shapes
	ShapeToModule.Add(TEXT("grid"),          TEXT("/Niagara/Modules/Spawn/Location/GridLocation.GridLocation"));
	ShapeToModule.Add(TEXT("gridlocation"),  TEXT("/Niagara/Modules/Spawn/Location/GridLocation.GridLocation"));
	ShapeToModule.Add(TEXT("gridv2"),        TEXT("/Niagara/Modules/Spawn/Location/V2/GridLocation.GridLocation"));
	ShapeToModule.Add(TEXT("line"),          TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("ring"),          TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("disc"),          TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("disk"),          TEXT("/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"));
	ShapeToModule.Add(TEXT("wedge"),         TEXT("/Niagara/Modules/Math/WedgeLocation.WedgeLocation"));
	ShapeToModule.Add(TEXT("curlnoise"),     TEXT("/Niagara/Modules/Spawn/Location/CurlNoiseLocation.CurlNoiseLocation"));
	ShapeToModule.Add(TEXT("skelmesh"),      TEXT("/Niagara/Modules/Spawn/Location/SkeletalMeshSurfaceLocation.SkeletalMeshSurfaceLocation"));
	ShapeToModule.Add(TEXT("staticmesh"),    TEXT("/Niagara/Modules/Spawn/Location/StaticMeshLocation.StaticMeshLocation"));

	FString* ModulePath = ShapeToModule.Find(Shape);
	if (!ModulePath)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid shape '%s'. Valid values: Cylinder, Sphere, Box, Cone, Torus, Grid, GridV2, Line, Ring, Disc, Wedge, CurlNoise, SkelMesh, StaticMesh, Shape (V2), ShapeLocation"), *Shape));

	TArray<FString> Warnings;

	// Derive the expected module name substring from the target shape
	// (e.g. "sphere" -> "SphereLocation", "shape" -> "ShapeLocation")
	FString TargetModuleKeyword;
	{
		FString ShapeCap = Shape;
		if (ShapeCap.Len() > 0) ShapeCap[0] = FChar::ToUpper(ShapeCap[0]);
		if (Shape == TEXT("shapev2") || Shape == TEXT("shapelocation")) TargetModuleKeyword = TEXT("ShapeLocation");
		else TargetModuleKeyword = ShapeCap + TEXT("Location");
	}

	// -------------------------------------------------------------------------
	// Step 1: Remove existing location modules if replace_existing is true
	// If the exact same shape module already exists, skip the whole operation.
	// -------------------------------------------------------------------------
	if (bReplaceExisting)
	{
		TSharedPtr<FJsonObject> GetModParams = MakeShared<FJsonObject>();
		GetModParams->SetStringField(TEXT("asset_path"), AssetPath);
		GetModParams->SetStringField(TEXT("emitter"), EmitterName);
		GetModParams->SetStringField(TEXT("usage"), TEXT("particle_spawn"));

		FMonolithActionResult ModResult = HandleGetOrderedModules(GetModParams);
		if (ModResult.bSuccess && ModResult.Result.IsValid())
		{
			// Known location module name substrings (covers individual + ShapeLocation V2)
			TArray<FString> LocationKeywords = {
				TEXT("CylinderLocation"), TEXT("SphereLocation"), TEXT("BoxLocation"),
				TEXT("ConeLocation"), TEXT("TorusLocation"), TEXT("ShapeLocation")
			};

			const TArray<TSharedPtr<FJsonValue>>* ModulesArr;
			if (ModResult.Result->TryGetArrayField(TEXT("modules"), ModulesArr))
			{
				bool bSameShapeAlreadyPresent = false;
				for (const TSharedPtr<FJsonValue>& ModVal : *ModulesArr)
				{
					const TSharedPtr<FJsonObject> Mod = ModVal->AsObject();
					if (!Mod.IsValid()) continue;

					FString ModName = Mod->GetStringField(TEXT("module_name"));
					FString NodeGuid = Mod->GetStringField(TEXT("node_guid"));

					bool bIsLocationModule = false;
					for (const FString& Keyword : LocationKeywords)
					{
						if (ModName.Contains(Keyword))
						{
							bIsLocationModule = true;
							// Check if this is the SAME shape we're trying to add
							if (ModName.Contains(TargetModuleKeyword))
							{
								bSameShapeAlreadyPresent = true;
							}
							else
							{
								// Different location module — remove it
								TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
								RemoveParams->SetStringField(TEXT("asset_path"), AssetPath);
								RemoveParams->SetStringField(TEXT("emitter"), EmitterName);
								RemoveParams->SetStringField(TEXT("module_node"), NodeGuid);
								HandleRemoveModule(RemoveParams);
								Warnings.Add(FString::Printf(TEXT("Removed existing location module: %s"), *ModName));
							}
							break;
						}
					}
				}

				// If the exact same shape is already present, skip — nothing to do
				if (bSameShapeAlreadyPresent)
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("message"), FString::Printf(TEXT("Shape '%s' is already set on emitter '%s' — no changes needed"), *Shape, *EmitterName));
					R->SetBoolField(TEXT("skipped"), true);
					return NA_SuccessObj(R);
				}
			}
		}
	}

	// -------------------------------------------------------------------------
	// Step 2: Set InitializeParticle's Position Mode to "Simulation Position"
	// Pre-check current value to avoid unnecessary switch change + recompile race
	// -------------------------------------------------------------------------
	{
		TSharedPtr<FJsonObject> GetModParams = MakeShared<FJsonObject>();
		GetModParams->SetStringField(TEXT("asset_path"), AssetPath);
		GetModParams->SetStringField(TEXT("emitter"), EmitterName);
		GetModParams->SetStringField(TEXT("usage"), TEXT("particle_spawn"));

		FMonolithActionResult ModResult = HandleGetOrderedModules(GetModParams);
		if (ModResult.bSuccess && ModResult.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ModulesArr;
			if (ModResult.Result->TryGetArrayField(TEXT("modules"), ModulesArr))
			{
				for (const TSharedPtr<FJsonValue>& ModVal : *ModulesArr)
				{
					const TSharedPtr<FJsonObject> Mod = ModVal->AsObject();
					if (!Mod.IsValid()) continue;

					FString ModName = Mod->GetStringField(TEXT("module_name"));
					if (ModName.Contains(TEXT("InitializeParticle")))
					{
						FString NodeGuid = Mod->GetStringField(TEXT("node_guid"));

						// Pre-check: read the current static switch pin value to avoid
						// unnecessary switch change that triggers a compilation race
						UNiagaraSystem* CheckSystem = LoadSystem(AssetPath);
						UNiagaraNodeFunctionCall* InitMN = CheckSystem ? FindModuleNode(CheckSystem, EmitterName, NodeGuid) : nullptr;
						bool bAlreadySimPos = false;
						if (InitMN)
						{
							for (UEdGraphPin* Pin : InitMN->Pins)
							{
								if (Pin->Direction == EGPD_Input && Pin->GetFName().ToString() == TEXT("Position Mode"))
								{
									if (Pin->DefaultValue.Contains(TEXT("Simulation Position")))
									{
										bAlreadySimPos = true;
									}
									break;
								}
							}
						}

						if (!bAlreadySimPos)
						{
							TSharedPtr<FJsonObject> SwitchParams = MakeShared<FJsonObject>();
							SwitchParams->SetStringField(TEXT("asset_path"), AssetPath);
							SwitchParams->SetStringField(TEXT("emitter"), EmitterName);
							SwitchParams->SetStringField(TEXT("module_node"), NodeGuid);
							SwitchParams->SetStringField(TEXT("input"), TEXT("Position Mode"));
							SwitchParams->SetStringField(TEXT("value"), TEXT("Simulation Position"));
							FMonolithActionResult SwitchResult = HandleSetStaticSwitchValue(SwitchParams);
							if (!SwitchResult.bSuccess)
							{
								Warnings.Add(FString::Printf(TEXT("Could not set InitializeParticle Position Mode: %s"), *SwitchResult.ErrorMessage));
							}
							else
							{
								// Sync compile after static switch change to avoid race
								// where subsequent module adds see stale ParameterMap state
								if (CheckSystem)
								{
									CheckSystem->RequestCompile(false);
									CheckSystem->WaitForCompilationComplete();
								}
							}
						}
						break;
					}
				}
			}
		}
	}

	// -------------------------------------------------------------------------
	// Step 3: Add the location module to particle_spawn
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), AssetPath);
	AddParams->SetStringField(TEXT("emitter"), EmitterName);
	AddParams->SetStringField(TEXT("module_script"), *ModulePath);
	AddParams->SetStringField(TEXT("usage"), TEXT("particle_spawn"));

	FMonolithActionResult AddResult = HandleAddModule(AddParams);
	if (!AddResult.bSuccess)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to add %s module: %s"), *Shape, *AddResult.ErrorMessage));

	FString NodeGuid;
	if (AddResult.Result.IsValid())
		NodeGuid = AddResult.Result->GetStringField(TEXT("node_guid"));

	// -------------------------------------------------------------------------
	// Step 4: Apply shape parameters if provided
	// -------------------------------------------------------------------------
	TArray<FString> ParamsSet;
	const TSharedPtr<FJsonObject>* ShapeParamsObj;
	if (!NodeGuid.IsEmpty() && Params->TryGetObjectField(TEXT("params"), ShapeParamsObj) && ShapeParamsObj->IsValid())
	{
		for (const auto& Pair : (*ShapeParamsObj)->Values)
		{
			// Map friendly param names to module input display names
			FString InputName = MonolithKeyToString(Pair.Key);
			if (InputName == TEXT("radius"))
			{
				if      (Shape == TEXT("cylinder")) InputName = TEXT("Cylinder Radius");
				else if (Shape == TEXT("sphere"))   InputName = TEXT("Sphere Radius");
				else if (Shape == TEXT("torus"))    InputName = TEXT("Torus Radius");
			}
			else if (InputName == TEXT("height"))
			{
				if      (Shape == TEXT("cylinder")) InputName = TEXT("Cylinder Height");
				else if (Shape == TEXT("cone"))     InputName = TEXT("Cone Height");
			}
			else if (InputName == TEXT("half_extent") && Shape == TEXT("box"))
			{
				InputName = TEXT("Box Half Extents");
			}
			// All other keys (surface_only, inner_radius, etc.) are passed through as-is

			TSharedPtr<FJsonObject> SetParams = MakeShared<FJsonObject>();
			SetParams->SetStringField(TEXT("asset_path"), AssetPath);
			SetParams->SetStringField(TEXT("emitter"), EmitterName);
			SetParams->SetStringField(TEXT("module_node"), NodeGuid);
			SetParams->SetStringField(TEXT("input"), InputName);
			SetParams->SetField(TEXT("value"), Pair.Value);

			FMonolithActionResult SetResult = HandleSetModuleInputValue(SetParams);
			if (SetResult.bSuccess)
				ParamsSet.Add(MonolithKeyToString(Pair.Key));
			else
				Warnings.Add(FString::Printf(TEXT("Failed to set param '%s': %s"), *MonolithKeyToString(Pair.Key), *SetResult.ErrorMessage));
		}
	}

	// -------------------------------------------------------------------------
	// Step 5: Compile
	// -------------------------------------------------------------------------
	TSharedPtr<FJsonObject> CompileParams = MakeShared<FJsonObject>();
	CompileParams->SetStringField(TEXT("asset_path"), AssetPath);
	HandleRequestCompile(CompileParams);

	// -------------------------------------------------------------------------
	// Build result
	// -------------------------------------------------------------------------
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("shape"), Shape);
	Result->SetStringField(TEXT("module_node_guid"), NodeGuid);
	Result->SetStringField(TEXT("location_module"), *ModulePath);

	TArray<TSharedPtr<FJsonValue>> ParamsArr;
	for (const FString& P : ParamsSet)
		ParamsArr.Add(MakeShared<FJsonValueString>(P));
	Result->SetArrayField(TEXT("params_set"), ParamsArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings)
			WarnArr.Add(MakeShared<FJsonValueString>(W));
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Phase 3: Dynamic Input Features (5 new actions)
// ============================================================================

// --------------------------------------------------------------------------
// Task 8: list_dynamic_inputs — flat list of dynamic inputs on a module
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleListDynamicInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// Get module inputs
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

	// CustomHlsl fallback
	if (Inputs.Num() == 0)
	{
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

	TArray<TSharedPtr<FJsonValue>> DynArr;
	for (const FNiagaraVariable& Input : Inputs)
	{
		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(Input.GetName()), MN);
		UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*MN, AH);
		if (!OP || OP->LinkedTo.Num() == 0) continue;

		UNiagaraNodeFunctionCall* DynNode = Cast<UNiagaraNodeFunctionCall>(OP->LinkedTo[0]->GetOwningNode());
		if (!DynNode) continue;

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("input_name"), MonolithNiagaraHelpers::StripModulePrefix(Input.GetName()).ToString());
		Entry->SetStringField(TEXT("dynamic_input_name"), DynNode->GetFunctionName());
		Entry->SetStringField(TEXT("dynamic_input_guid"), DynNode->NodeGuid.ToString());
		if (DynNode->FunctionScript)
			Entry->SetStringField(TEXT("script_path"), DynNode->FunctionScript->GetPathName());
		DynArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), DynArr.Num());
	R->SetArrayField(TEXT("dynamic_inputs"), DynArr);
	return NA_SuccessObj(R);
}

// --------------------------------------------------------------------------
// Task 9: get_dynamic_input_tree — recursive tree of all module inputs
// --------------------------------------------------------------------------

namespace
{
	TSharedPtr<FJsonObject> BuildInputTreeNode(
		UNiagaraNodeFunctionCall* FuncNode,
		UNiagaraSystem* System,
		int32 EmitterIdx,
		ENiagaraScriptUsage Usage,
		int32 Depth,
		int32 MaxDepth)
	{
		if (!FuncNode || Depth > MaxDepth) return nullptr;

		TSharedRef<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("function_name"), FuncNode->GetFunctionName());
		NodeObj->SetStringField(TEXT("node_guid"), FuncNode->NodeGuid.ToString());
		if (FuncNode->FunctionScript)
			NodeObj->SetStringField(TEXT("script_path"), FuncNode->FunctionScript->GetPathName());

		// Get inputs for this function node
		TArray<FNiagaraVariable> Inputs;
		if (EmitterIdx != INDEX_NONE)
		{
			FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
			FCompileConstantResolver Resolver(VE, Usage);
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(*FuncNode, Inputs, Resolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
		}
		else
		{
			FCompileConstantResolver Resolver(System, Usage);
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(*FuncNode, Inputs, Resolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
		}

		// CustomHlsl fallback
		if (Inputs.Num() == 0)
		{
			const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
			for (UEdGraphPin* Pin : FuncNode->Pins)
			{
				if (Pin->Direction != EGPD_Input) continue;
				FNiagaraTypeDefinition PinType = Schema->PinToTypeDefinition(Pin);
				if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
				if (Pin->PinName.IsNone() || Pin->PinName == TEXT("Add")) continue;
				Inputs.Add(FNiagaraVariable(PinType, Pin->PinName));
			}
		}

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const FNiagaraVariable& Input : Inputs)
		{
			TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
			FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(Input.GetName());
			IO->SetStringField(TEXT("name"), ShortName.ToString());
			IO->SetStringField(TEXT("type"), Input.GetType().GetName());

			FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				FNiagaraParameterHandle(Input.GetName()), FuncNode);
			UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*FuncNode, AH);

			if (!OP || OP->LinkedTo.Num() == 0)
			{
				// Literal / default value
				IO->SetStringField(TEXT("source"), TEXT("literal"));
				if (OP) IO->SetStringField(TEXT("value"), OP->DefaultValue);
			}
			else
			{
				UEdGraphNode* LinkedNode = OP->LinkedTo[0]->GetOwningNode();
				if (UNiagaraNodeFunctionCall* SubDyn = Cast<UNiagaraNodeFunctionCall>(LinkedNode))
				{
					// Recursive dynamic input
					IO->SetStringField(TEXT("source"), TEXT("dynamic_input"));
					TSharedPtr<FJsonObject> SubTree = BuildInputTreeNode(SubDyn, System, EmitterIdx, Usage, Depth + 1, MaxDepth);
					if (SubTree.IsValid())
						IO->SetObjectField(TEXT("dynamic_input"), SubTree);
				}
				else if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
				{
					// Parameter binding or data interface
					if (Input.GetType().IsDataInterface())
					{
						IO->SetStringField(TEXT("source"), TEXT("data_interface"));
						IO->SetStringField(TEXT("linked_parameter"), InputNode->Input.GetName().ToString());
					}
					else
					{
						IO->SetStringField(TEXT("source"), TEXT("parameter_binding"));
						IO->SetStringField(TEXT("linked_parameter"), InputNode->Input.GetName().ToString());
					}
				}
				else
				{
					IO->SetStringField(TEXT("source"), TEXT("linked"));
					IO->SetStringField(TEXT("linked_node_class"), LinkedNode->GetClass()->GetName());
				}
			}

			InputsArr.Add(MakeShared<FJsonValueObject>(IO));
		}
		NodeObj->SetArrayField(TEXT("inputs"), InputsArr);
		return NodeObj;
	}
} // anonymous namespace

FMonolithActionResult FMonolithNiagaraActions::HandleGetDynamicInputTree(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	int32 MaxDepth = Params->HasField(TEXT("max_depth")) ? static_cast<int32>(Params->GetNumberField(TEXT("max_depth"))) : 10;

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);

	TSharedPtr<FJsonObject> Tree = BuildInputTreeNode(MN, System, EmitterIdx, FoundUsage, 0, MaxDepth);
	if (!Tree.IsValid()) return FMonolithActionResult::Error(TEXT("Failed to build input tree"));

	return NA_SuccessObj(Tree.ToSharedRef());
}

// --------------------------------------------------------------------------
// Task 10: remove_dynamic_input — remove DI from a module pin
// --------------------------------------------------------------------------

namespace
{
	// Recursively remove a dynamic input node and all its sub-nodes from the graph.
	void RemoveDynamicInputFromPin(UEdGraphPin& OverridePin, UEdGraph* Graph)
	{
		if (!Graph || OverridePin.LinkedTo.Num() == 0) return;

		UEdGraphNode* LinkedNode = OverridePin.LinkedTo[0]->GetOwningNode();
		if (!LinkedNode) return;

		if (UNiagaraNodeFunctionCall* DynNode = Cast<UNiagaraNodeFunctionCall>(LinkedNode))
		{
			// Find its override node and recursively clean sub-inputs
			UEdGraphNode* DynOverrideNode = MonolithNiagaraHelpers::GetStackFunctionOverrideNode(*DynNode);
			if (DynOverrideNode)
			{
				TArray<UEdGraphPin*> SubPins;
				MonolithNiagaraHelpers::GetOverridePinsForFunction(DynOverrideNode, *DynNode, SubPins);
				for (UEdGraphPin* SubPin : SubPins)
				{
					if (SubPin && SubPin->LinkedTo.Num() > 0)
					{
						RemoveDynamicInputFromPin(*SubPin, Graph);
					}
				}
				// Remove the override node if it only has PM pins left (no other function's pins)
				bool bHasOtherPins = false;
				for (UEdGraphPin* Pin : DynOverrideNode->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					UNiagaraNode* NN = Cast<UNiagaraNode>(DynOverrideNode);
					if (NN && Pin == MonolithNiagaraHelpers::GetParameterMapPin(*NN, EGPD_Input)) continue;
					if (Pin->PinName == TEXT("Add")) continue;
					// Check if this pin belongs to any still-alive function (not the one we're removing)
					FNiagaraParameterHandle Handle(Pin->PinName);
					if (Handle.GetNamespace().ToString() != DynNode->GetFunctionName())
					{
						bHasOtherPins = true;
						break;
					}
				}
				if (!bHasOtherPins)
				{
					// Splice override node out of PM chain before removing
					UNiagaraNode* OvNN = Cast<UNiagaraNode>(DynOverrideNode);
					if (OvNN)
					{
						UEdGraphPin* OvMapIn = MonolithNiagaraHelpers::GetParameterMapPin(*OvNN, EGPD_Input);
						UEdGraphPin* OvMapOut = MonolithNiagaraHelpers::GetParameterMapPin(*OvNN, EGPD_Output);
						if (OvMapIn && OvMapOut && OvMapIn->LinkedTo.Num() > 0)
						{
							UEdGraphPin* UpstreamOut = OvMapIn->LinkedTo[0];
							OvMapIn->BreakAllPinLinks();
							// Reconnect upstream to downstream
							TArray<UEdGraphPin*> Downstream = OvMapOut->LinkedTo;
							OvMapOut->BreakAllPinLinks();
							for (UEdGraphPin* DownPin : Downstream)
							{
								if (UpstreamOut && DownPin)
									UpstreamOut->MakeLinkTo(DownPin);
							}
						}
						else
						{
							DynOverrideNode->BreakAllNodeLinks();
						}
					}
					Graph->RemoveNode(DynOverrideNode);
				}
			}
			// Remove the dynamic input function call node
			DynNode->BreakAllNodeLinks();
			Graph->RemoveNode(DynNode);
		}
		else if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
		{
			InputNode->BreakAllNodeLinks();
			Graph->RemoveNode(InputNode);
		}

		// Clean the override pin link
		OverridePin.BreakAllPinLinks();
	}
} // anonymous namespace

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveDynamicInput(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	FString InputName = Params->GetStringField(TEXT("input"));
	FString DynNodeGuid = Params->GetStringField(TEXT("dynamic_input_node"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UEdGraphPin* OverridePin = nullptr;
	UEdGraph* Graph = nullptr;

	if (!DynNodeGuid.IsEmpty())
	{
		// Mode 2: find by dynamic input node GUID — locate the pin it's connected to
		UNiagaraNodeFunctionCall* DynNode = FindFunctionCallNode(System, EmitterHandleId, DynNodeGuid);
		if (!DynNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Dynamic input node '%s' not found"), *DynNodeGuid));

		Graph = DynNode->GetGraph();

		// Find the override pin that links to this DynNode — walk DynNode's PM input pin upstream
		// The DynNode's output is connected to an override pin (or the module's override pin)
		for (UEdGraphPin* Pin : DynNode->Pins)
		{
			if (Pin->Direction != EGPD_Output) continue;
			FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
			// This is the data output pin — check what it's linked to
			if (Pin->LinkedTo.Num() > 0)
			{
				// The linked pin is the override pin on the upstream override node
				OverridePin = Pin->LinkedTo[0];
			}
			if (OverridePin) break;
		}
		if (!OverridePin) return FMonolithActionResult::Error(TEXT("Could not find the override pin connected to this dynamic input"));
	}
	else if (!ModuleNodeGuid.IsEmpty() && !InputName.IsEmpty())
	{
		// Mode 1: module_node + input
		ENiagaraScriptUsage FoundUsage;
		UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
		if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

		Graph = MN->GetGraph();

		// Find the input
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

		// CustomHlsl fallback
		if (Inputs.Num() == 0)
		{
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

		FName InputFName(*InputName);
		FString InputNoSpaces = InputName;
		InputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
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
			if (bMatch) { MatchedFullName = In.GetName(); bInputFound = true; break; }
		}
		if (!bInputFound) return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found"), *InputName));

		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(MatchedFullName), MN);
		OverridePin = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*MN, AH);
		if (!OverridePin) return FMonolithActionResult::Error(TEXT("No override pin found for this input — no dynamic input attached"));
		if (OverridePin->LinkedTo.Num() == 0) return FMonolithActionResult::Error(TEXT("Input has no dynamic input attached"));

		// Verify it's actually a dynamic input (function call), not a parameter binding
		if (!Cast<UNiagaraNodeFunctionCall>(OverridePin->LinkedTo[0]->GetOwningNode()))
			return FMonolithActionResult::Error(TEXT("Input is linked to a parameter binding, not a dynamic input"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Provide either (module_node + input) or dynamic_input_node"));
	}

	if (!OverridePin || !Graph)
		return FMonolithActionResult::Error(TEXT("Failed to resolve override pin or graph"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemoveDynInput", "Remove Dynamic Input"));
	System->Modify();

	RemoveDynamicInputFromPin(*OverridePin, Graph);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	return NA_SuccessObj(R);
}

// --------------------------------------------------------------------------
// Task 11: get_dynamic_input_value — read a value from a DI sub-pin
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleGetDynamicInputValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString DynNodeGuid = Params->GetStringField(TEXT("dynamic_input_node"));
	FString InputName = Params->GetStringField(TEXT("input"));

	if (DynNodeGuid.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: dynamic_input_node"));
	if (InputName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: input"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraNodeFunctionCall* DynNode = FindFunctionCallNode(System, EmitterHandleId, DynNodeGuid);
	if (!DynNode) return FMonolithActionResult::Error(FString::Printf(TEXT("Dynamic input node '%s' not found"), *DynNodeGuid));

	// Determine usage for the resolver — walk PM chain to find the output node
	ENiagaraScriptUsage FoundUsage = ENiagaraScriptUsage::ParticleUpdateScript; // sensible default
	{
		// Walk PM output chain to find the output node and infer usage
		UNiagaraNode* Current = DynNode;
		for (int32 Safety = 0; Safety < 100 && Current; ++Safety)
		{
			if (UNiagaraNodeOutput* OutNode = Cast<UNiagaraNodeOutput>(Current))
			{
				FoundUsage = OutNode->GetUsage();
				break;
			}
			UEdGraphPin* MapOut = MonolithNiagaraHelpers::GetParameterMapPin(*Current, EGPD_Output);
			if (!MapOut || MapOut->LinkedTo.Num() == 0) break;
			Current = Cast<UNiagaraNode>(MapOut->LinkedTo[0]->GetOwningNode());
		}
	}

	// Get inputs using GetStackFunctionInputs
	TArray<FNiagaraVariable> Inputs;
	int32 EmitterIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[EmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*DynNode, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, FoundUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*DynNode, Inputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	// CustomHlsl fallback
	if (Inputs.Num() == 0)
	{
		const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
		for (UEdGraphPin* Pin : DynNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			FNiagaraTypeDefinition PinType = Schema->PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
			if (Pin->PinName.IsNone() || Pin->PinName == TEXT("Add")) continue;
			Inputs.Add(FNiagaraVariable(PinType, Pin->PinName));
		}
	}

	// Find the specific input
	FName InputFName(*InputName);
	FString InputNoSpaces = InputName;
	InputNoSpaces.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
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
		if (!bMatch) continue;

		FNiagaraParameterHandle AH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(In.GetName()), DynNode);
		UEdGraphPin* OP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*DynNode, AH);

		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("input"), Short.ToString());
		R->SetStringField(TEXT("type"), In.GetType().GetName());

		if (OP)
		{
			R->SetBoolField(TEXT("has_override"), true);
			if (OP->LinkedTo.Num() > 0)
			{
				R->SetBoolField(TEXT("is_linked"), true);
				if (UNiagaraNodeInput* LI = Cast<UNiagaraNodeInput>(OP->LinkedTo[0]->GetOwningNode()))
					R->SetStringField(TEXT("linked_parameter"), LI->Input.GetName().ToString());
				else if (UNiagaraNodeFunctionCall* SubDyn = Cast<UNiagaraNodeFunctionCall>(OP->LinkedTo[0]->GetOwningNode()))
				{
					R->SetStringField(TEXT("source"), TEXT("dynamic_input"));
					R->SetStringField(TEXT("dynamic_input_name"), SubDyn->GetFunctionName());
					R->SetStringField(TEXT("dynamic_input_guid"), SubDyn->NodeGuid.ToString());
				}
			}
			else
			{
				R->SetStringField(TEXT("value"), OP->DefaultValue);
				R->SetBoolField(TEXT("is_default"), OP->DefaultValue.IsEmpty());
			}
		}
		else
		{
			R->SetBoolField(TEXT("has_override"), false);
			R->SetBoolField(TEXT("is_default"), true);
		}

		return NA_SuccessObj(R);
	}

	// Build list of valid input names for the error message
	TArray<FString> ValidNames;
	for (const FNiagaraVariable& In : Inputs)
		ValidNames.Add(MonolithNiagaraHelpers::StripModulePrefix(In.GetName()).ToString());

	return FMonolithActionResult::Error(FString::Printf(
		TEXT("Input '%s' not found on dynamic input node. Available: %s"),
		*InputName, *FString::Join(ValidNames, TEXT(", "))));
}

// --------------------------------------------------------------------------
// Task 12: get_dynamic_input_inputs — discover inputs on an unattached script
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleGetDynamicInputInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	if (ScriptPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: script_path"));

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load script: %s"), *ScriptPath));

	UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Src || !Src->NodeGraph) return FMonolithActionResult::Error(TEXT("Script has no graph"));

	UNiagaraGraph* Graph = Src->NodeGraph;

	// Find input nodes (parameters)
	TArray<UNiagaraNodeInput*> InputNodes;
	Graph->GetNodesOfClass<UNiagaraNodeInput>(InputNodes);

	TArray<TSharedPtr<FJsonValue>> InputsArr;
	for (UNiagaraNodeInput* InputNode : InputNodes)
	{
		if (!InputNode) continue;
		if (InputNode->Usage != ENiagaraInputNodeUsage::Parameter) continue;

		TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(InputNode->Input.GetName());
		IO->SetStringField(TEXT("name"), ShortName.ToString());
		IO->SetStringField(TEXT("full_name"), InputNode->Input.GetName().ToString());
		IO->SetStringField(TEXT("type"), InputNode->Input.GetType().GetName());
		IO->SetBoolField(TEXT("is_data_interface"), InputNode->Input.GetType().IsDataInterface());
		InputsArr.Add(MakeShared<FJsonValueObject>(IO));
	}

	// Find output type from the output node
	FString OutputType;
	TArray<UNiagaraNodeOutput*> OutputNodes;
	Graph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
	for (UNiagaraNodeOutput* OutNode : OutputNodes)
	{
		if (!OutNode) continue;
		for (UEdGraphPin* Pin : OutNode->Pins)
		{
			if (Pin->Direction != EGPD_Input) continue;
			FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
			if (PinType == FNiagaraTypeDefinition::GetParameterMapDef()) continue;
			OutputType = PinType.GetName();
			break;
		}
		if (!OutputType.IsEmpty()) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("script_path"), ScriptPath);
	R->SetStringField(TEXT("script_name"), Script->GetName());
	R->SetStringField(TEXT("output_type"), OutputType);
	R->SetNumberField(TEXT("input_count"), InputsArr.Num());
	R->SetArrayField(TEXT("inputs"), InputsArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 5: Renderer & DI Improvements (4 new actions)
// ============================================================================

// --------------------------------------------------------------------------
// Task 16: list_available_renderers — static catalog of renderer types
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleListAvailableRenderers(const TSharedPtr<FJsonObject>& Params)
{
	struct FRendererInfo
	{
		const TCHAR* ShortName;
		const TCHAR* ClassName;
		const TCHAR* Description;
		bool bCPUOnly;
	};

	static const FRendererInfo Renderers[] = {
		{ TEXT("sprite"),    TEXT("NiagaraSpriteRendererProperties"),    TEXT("Billboard sprite particles"),              false },
		{ TEXT("mesh"),      TEXT("NiagaraMeshRendererProperties"),      TEXT("Instanced static mesh particles"),         false },
		{ TEXT("ribbon"),    TEXT("NiagaraRibbonRendererProperties"),    TEXT("Ribbon/trail rendering"),                  false },
		{ TEXT("light"),     TEXT("NiagaraLightRendererProperties"),     TEXT("Per-particle point lights"),               false },
		{ TEXT("component"), TEXT("NiagaraComponentRendererProperties"), TEXT("Arbitrary UActorComponent per particle"),  false },
		{ TEXT("decal"),     TEXT("NiagaraDecalRendererProperties"),     TEXT("Per-particle decal projection"),           true  },
		{ TEXT("volume"),    TEXT("NiagaraVolumeRendererProperties"),    TEXT("Volumetric fog/cloud rendering"),          false },
	};

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FRendererInfo& Info : Renderers)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("short_name"), Info.ShortName);
		Entry->SetStringField(TEXT("class_name"), Info.ClassName);
		Entry->SetStringField(TEXT("description"), Info.Description);
		Entry->SetBoolField(TEXT("cpu_only"), Info.bCPUOnly);
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Arr.Num());
	R->SetArrayField(TEXT("renderers"), Arr);
	return NA_SuccessObj(R);
}

// --------------------------------------------------------------------------
// Task 17: set_renderer_mesh — assign StaticMesh to a MeshRenderer slot
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleSetRendererMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString MeshPath = Params->GetStringField(TEXT("mesh"));
	if (MeshPath.IsEmpty()) MeshPath = Params->GetStringField(TEXT("mesh_path"));
	int32 MeshIndex = Params->HasField(TEXT("mesh_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("mesh_index"))) : 0;

	if (MeshPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: mesh"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	UNiagaraMeshRendererProperties* MeshRend = Cast<UNiagaraMeshRendererProperties>(Rend);
	if (!MeshRend) return FMonolithActionResult::Error(TEXT("Renderer is not a MeshRenderer. Use add_renderer with class 'mesh' first."));

	UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!SM) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load StaticMesh: %s"), *MeshPath));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetRendMesh", "Set Renderer Mesh"));
	System->Modify();
	MeshRend->Modify();

	// Grow array if needed
	if (MeshRend->Meshes.Num() <= MeshIndex)
	{
		MeshRend->Meshes.SetNum(MeshIndex + 1);
	}

	MeshRend->Meshes[MeshIndex].Mesh = SM;

	// Optional scale
	const TSharedPtr<FJsonObject>* ScaleObj;
	if (Params->TryGetObjectField(TEXT("scale"), ScaleObj) && ScaleObj->IsValid())
	{
		MeshRend->Meshes[MeshIndex].Scale = FVector(
			(*ScaleObj)->GetNumberField(TEXT("x")),
			(*ScaleObj)->GetNumberField(TEXT("y")),
			(*ScaleObj)->GetNumberField(TEXT("z")));
	}

	// Optional rotation
	const TSharedPtr<FJsonObject>* RotObj;
	if (Params->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj->IsValid())
	{
		MeshRend->Meshes[MeshIndex].Rotation = FRotator(
			(*RotObj)->HasField(TEXT("pitch")) ? (*RotObj)->GetNumberField(TEXT("pitch")) : 0.0,
			(*RotObj)->HasField(TEXT("yaw"))   ? (*RotObj)->GetNumberField(TEXT("yaw"))   : 0.0,
			(*RotObj)->HasField(TEXT("roll"))  ? (*RotObj)->GetNumberField(TEXT("roll"))  : 0.0);
	}

	// Optional pivot offset
	const TSharedPtr<FJsonObject>* PivotObj;
	if (Params->TryGetObjectField(TEXT("pivot_offset"), PivotObj) && PivotObj->IsValid())
	{
		MeshRend->Meshes[MeshIndex].PivotOffset = FVector(
			(*PivotObj)->GetNumberField(TEXT("x")),
			(*PivotObj)->GetNumberField(TEXT("y")),
			(*PivotObj)->GetNumberField(TEXT("z")));
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("mesh"), MeshPath);
	R->SetNumberField(TEXT("mesh_index"), MeshIndex);
	R->SetNumberField(TEXT("total_mesh_slots"), MeshRend->Meshes.Num());
	return NA_SuccessObj(R);
}

// --------------------------------------------------------------------------
// Task 19: configure_ribbon — high-level ribbon/trail/beam setup
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleConfigureRibbon(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	FString Preset = Params->HasField(TEXT("preset")) ? Params->GetStringField(TEXT("preset")).ToLower() : FString();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	UNiagaraRibbonRendererProperties* RibbonRend = Cast<UNiagaraRibbonRendererProperties>(Rend);
	if (!RibbonRend) return FMonolithActionResult::Error(TEXT("Renderer is not a RibbonRenderer. Use add_renderer with class 'ribbon' first."));

	// Start with preset defaults, then override with explicit params
	FString ShapeStr, FacingStr, TessModeStr, UVModeStr;
	int32 TessFactor = -1;
	int32 TubeSubdivisions = -1;
	float TilingLength = -1.f;

	// Apply preset defaults
	if (Preset == TEXT("trail"))
	{
		ShapeStr = TEXT("Plane");
		FacingStr = TEXT("Screen");
		TessModeStr = TEXT("Automatic");
		UVModeStr = TEXT("TiledOverRibbonLength");
	}
	else if (Preset == TEXT("beam"))
	{
		ShapeStr = TEXT("Plane");
		TessModeStr = TEXT("Custom");
		TessFactor = 8;
		UVModeStr = TEXT("ScaledUniformly");
	}
	else if (Preset == TEXT("lightning"))
	{
		ShapeStr = TEXT("Plane");
		TessModeStr = TEXT("Disabled");
		UVModeStr = TEXT("ScaledUniformly");
	}
	else if (Preset == TEXT("tube"))
	{
		ShapeStr = TEXT("Tube");
		TubeSubdivisions = 8;
		UVModeStr = TEXT("ScaledUniformly");
	}
	else if (!Preset.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown preset '%s'. Valid presets: trail, beam, lightning, tube"), *Preset));
	}

	// Override with explicit params
	if (Params->HasField(TEXT("shape")))        ShapeStr = Params->GetStringField(TEXT("shape"));
	if (Params->HasField(TEXT("facing_mode")))  FacingStr = Params->GetStringField(TEXT("facing_mode"));
	if (Params->HasField(TEXT("tessellation_mode"))) TessModeStr = Params->GetStringField(TEXT("tessellation_mode"));
	if (Params->HasField(TEXT("tessellation_factor"))) TessFactor = static_cast<int32>(Params->GetNumberField(TEXT("tessellation_factor")));
	if (Params->HasField(TEXT("tube_subdivisions"))) TubeSubdivisions = static_cast<int32>(Params->GetNumberField(TEXT("tube_subdivisions")));
	if (Params->HasField(TEXT("uv_mode")))      UVModeStr = Params->GetStringField(TEXT("uv_mode"));
	if (Params->HasField(TEXT("tiling_length"))) TilingLength = static_cast<float>(Params->GetNumberField(TEXT("tiling_length")));

	TArray<FString> PropsSet;
	TArray<FString> Warnings;

	// Helper lambda: delegate to HandleSetRendererProperty
	auto SetProp = [&](const FString& PropName, const FString& Value)
	{
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("asset_path"), SystemPath);
		SubParams->SetStringField(TEXT("emitter"), EmitterHandleId);
		SubParams->SetNumberField(TEXT("renderer_index"), RendererIndex);
		SubParams->SetStringField(TEXT("property"), PropName);
		SubParams->SetField(TEXT("value"), MakeShared<FJsonValueString>(Value));
		FMonolithActionResult R = HandleSetRendererProperty(SubParams);
		if (R.bSuccess)
			PropsSet.Add(PropName);
		else
			Warnings.Add(FString::Printf(TEXT("Failed to set %s: %s"), *PropName, *R.ErrorMessage));
	};

	// Helper lambda: delegate to HandleSetRendererBinding
	auto SetBinding = [&](const FString& BindingName, const FString& Attribute)
	{
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("asset_path"), SystemPath);
		SubParams->SetStringField(TEXT("emitter"), EmitterHandleId);
		SubParams->SetNumberField(TEXT("renderer_index"), RendererIndex);
		SubParams->SetStringField(TEXT("binding_name"), BindingName);
		SubParams->SetStringField(TEXT("attribute"), Attribute);
		FMonolithActionResult R = HandleSetRendererBinding(SubParams);
		if (R.bSuccess)
			PropsSet.Add(BindingName);
		else
			Warnings.Add(FString::Printf(TEXT("Failed to set binding %s: %s"), *BindingName, *R.ErrorMessage));
	};

	// Apply properties
	if (!ShapeStr.IsEmpty())     SetProp(TEXT("Shape"), ShapeStr);
	if (!FacingStr.IsEmpty())    SetProp(TEXT("FacingMode"), FacingStr);
	if (!TessModeStr.IsEmpty())  SetProp(TEXT("TessellationMode"), TessModeStr);
	if (TessFactor >= 0)         SetProp(TEXT("TessellationFactor"), FString::FromInt(TessFactor));
	if (TubeSubdivisions >= 0)   SetProp(TEXT("TubeSubdivisions"), FString::FromInt(TubeSubdivisions));

	// UV settings — set on UV0Settings struct via ImportText
	if (!UVModeStr.IsEmpty() || TilingLength >= 0.f)
	{
		FString UVStr = TEXT("(");
		bool bFirst = true;
		if (!UVModeStr.IsEmpty())
		{
			UVStr += FString::Printf(TEXT("DistributionMode=%s"), *UVModeStr);
			bFirst = false;
		}
		if (TilingLength >= 0.f)
		{
			if (!bFirst) UVStr += TEXT(",");
			UVStr += FString::Printf(TEXT("TilingLength=%f"), TilingLength);
		}
		UVStr += TEXT(")");
		SetProp(TEXT("UV0Settings"), UVStr);
	}

	// Binding overrides
	if (Params->HasField(TEXT("width_binding")))
		SetBinding(TEXT("RibbonWidthBinding"), Params->GetStringField(TEXT("width_binding")));
	if (Params->HasField(TEXT("link_order_binding")))
		SetBinding(TEXT("RibbonLinkOrderBinding"), Params->GetStringField(TEXT("link_order_binding")));
	if (Params->HasField(TEXT("ribbon_id_binding")))
		SetBinding(TEXT("RibbonIdBinding"), Params->GetStringField(TEXT("ribbon_id_binding")));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	if (!Preset.IsEmpty()) R->SetStringField(TEXT("preset"), Preset);

	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (const FString& P : PropsSet) PropsArr.Add(MakeShared<FJsonValueString>(P));
	R->SetArrayField(TEXT("properties_set"), PropsArr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		R->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return NA_SuccessObj(R);
}

// --------------------------------------------------------------------------
// Task 20: configure_subuv — flipbook animation setup on a sprite renderer
// --------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleConfigureSubUV(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	int32 RendererIndex = static_cast<int32>(Params->GetNumberField(TEXT("renderer_index")));
	int32 Columns = static_cast<int32>(Params->GetNumberField(TEXT("columns")));
	int32 Rows = static_cast<int32>(Params->GetNumberField(TEXT("rows")));
	bool bBlend = Params->HasField(TEXT("blend")) ? Params->GetBoolField(TEXT("blend")) : false;
	bool bAddModule = Params->HasField(TEXT("add_animation_module")) ? Params->GetBoolField(TEXT("add_animation_module")) : false;

	if (Columns <= 0 || Rows <= 0)
		return FMonolithActionResult::Error(TEXT("columns and rows must be positive integers"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	UNiagaraRendererProperties* Rend = GetRenderer(System, EmitterHandleId, RendererIndex);
	if (!Rend) return FMonolithActionResult::Error(TEXT("Renderer not found"));

	// SubImageSize exists on both Sprite and Mesh renderers — verify it's one of them
	UNiagaraSpriteRendererProperties* SpriteRend = Cast<UNiagaraSpriteRendererProperties>(Rend);
	UNiagaraMeshRendererProperties* MeshRend = Cast<UNiagaraMeshRendererProperties>(Rend);
	if (!SpriteRend && !MeshRend)
		return FMonolithActionResult::Error(TEXT("Renderer is not a SpriteRenderer or MeshRenderer"));

	TArray<FString> Warnings;

	// Step 1: Set SubImageSize via HandleSetRendererProperty
	{
		FString SubImageStr = FString::Printf(TEXT("(X=%d,Y=%d)"), Columns, Rows);
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("asset_path"), SystemPath);
		SubParams->SetStringField(TEXT("emitter"), EmitterHandleId);
		SubParams->SetNumberField(TEXT("renderer_index"), RendererIndex);
		SubParams->SetStringField(TEXT("property"), TEXT("SubImageSize"));
		SubParams->SetField(TEXT("value"), MakeShared<FJsonValueString>(SubImageStr));
		FMonolithActionResult R = HandleSetRendererProperty(SubParams);
		if (!R.bSuccess)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set SubImageSize: %s"), *R.ErrorMessage));
	}

	// Step 2: Set bSubImageBlend if requested
	if (bBlend)
	{
		TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
		SubParams->SetStringField(TEXT("asset_path"), SystemPath);
		SubParams->SetStringField(TEXT("emitter"), EmitterHandleId);
		SubParams->SetNumberField(TEXT("renderer_index"), RendererIndex);
		SubParams->SetStringField(TEXT("property"), TEXT("bSubImageBlend"));
		SubParams->SetField(TEXT("value"), MakeShared<FJsonValueBoolean>(true));
		FMonolithActionResult R = HandleSetRendererProperty(SubParams);
		if (!R.bSuccess)
			Warnings.Add(FString::Printf(TEXT("Failed to set bSubImageBlend: %s"), *R.ErrorMessage));
	}

	// Step 3: Add SubUVAnimation module if requested
	FString AnimModuleNodeGuid;
	if (bAddModule)
	{
		TSharedRef<FJsonObject> AddParams = MakeShared<FJsonObject>();
		AddParams->SetStringField(TEXT("asset_path"), SystemPath);
		AddParams->SetStringField(TEXT("emitter"), EmitterHandleId);
		AddParams->SetStringField(TEXT("module_script"), TEXT("/Niagara/Modules/Update/SubUV/SubUVAnimation.SubUVAnimation"));
		AddParams->SetStringField(TEXT("usage"), TEXT("particle_update"));
		FMonolithActionResult AddResult = HandleAddModule(AddParams);
		if (AddResult.bSuccess && AddResult.Result.IsValid())
		{
			AnimModuleNodeGuid = AddResult.Result->GetStringField(TEXT("node_guid"));

			// Set module inputs if specified
			if (!AnimModuleNodeGuid.IsEmpty())
			{
				if (Params->HasField(TEXT("playback_mode")))
				{
					TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
					SetParams->SetStringField(TEXT("asset_path"), SystemPath);
					SetParams->SetStringField(TEXT("emitter"), EmitterHandleId);
					SetParams->SetStringField(TEXT("module_node"), AnimModuleNodeGuid);
					SetParams->SetStringField(TEXT("input"), TEXT("Playback Mode"));
					SetParams->SetStringField(TEXT("value"), Params->GetStringField(TEXT("playback_mode")));
					FMonolithActionResult R = HandleSetStaticSwitchValue(SetParams);
					if (!R.bSuccess) Warnings.Add(FString::Printf(TEXT("Failed to set Playback Mode: %s"), *R.ErrorMessage));
				}
				if (Params->HasField(TEXT("start_frame")))
				{
					TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
					SetParams->SetStringField(TEXT("asset_path"), SystemPath);
					SetParams->SetStringField(TEXT("emitter"), EmitterHandleId);
					SetParams->SetStringField(TEXT("module_node"), AnimModuleNodeGuid);
					SetParams->SetStringField(TEXT("input"), TEXT("Start Frame"));
					SetParams->SetStringField(TEXT("value"), FString::SanitizeFloat(Params->GetNumberField(TEXT("start_frame"))));
					FMonolithActionResult R = HandleSetModuleInputValue(SetParams);
					if (!R.bSuccess) Warnings.Add(FString::Printf(TEXT("Failed to set Start Frame: %s"), *R.ErrorMessage));
				}
				if (Params->HasField(TEXT("end_frame")))
				{
					TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
					SetParams->SetStringField(TEXT("asset_path"), SystemPath);
					SetParams->SetStringField(TEXT("emitter"), EmitterHandleId);
					SetParams->SetStringField(TEXT("module_node"), AnimModuleNodeGuid);
					SetParams->SetStringField(TEXT("input"), TEXT("End Frame"));
					SetParams->SetStringField(TEXT("value"), FString::SanitizeFloat(Params->GetNumberField(TEXT("end_frame"))));
					FMonolithActionResult R = HandleSetModuleInputValue(SetParams);
					if (!R.bSuccess) Warnings.Add(FString::Printf(TEXT("Failed to set End Frame: %s"), *R.ErrorMessage));
				}
			}
		}
		else
		{
			Warnings.Add(FString::Printf(TEXT("Failed to add SubUVAnimation module: %s"), *AddResult.ErrorMessage));
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetNumberField(TEXT("columns"), Columns);
	R->SetNumberField(TEXT("rows"), Rows);
	R->SetBoolField(TEXT("blend"), bBlend);
	if (!AnimModuleNodeGuid.IsEmpty())
		R->SetStringField(TEXT("animation_module_guid"), AnimModuleNodeGuid);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		R->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 4: Module & Emitter Management (Tasks 14-15)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleRenameEmitter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString NewName = Params->GetStringField(TEXT("name"));

	if (NewName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param 'name'"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter handle not found"));

	FString OldName = System->GetEmitterHandles()[Index].GetName().ToString();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RenameEmitter", "Rename Emitter"));
	System->Modify();

	System->GetEmitterHandles()[Index].SetName(FName(*NewName), *System);

	// SetName may apply a uniqueness suffix if the name collides — read back the actual name
	FString ActualName = System->GetEmitterHandles()[Index].GetName().ToString();

	GEditor->EndTransaction();

	System->MarkPackageDirty();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("old_name"), OldName);
	R->SetStringField(TEXT("requested_name"), NewName);
	R->SetStringField(TEXT("actual_name"), ActualName);
	R->SetBoolField(TEXT("name_changed"), OldName != ActualName);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetEmitterProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString PropertyName = Params->GetStringField(TEXT("property"));
	if (PropertyName.IsEmpty()) PropertyName = Params->GetStringField(TEXT("property_name"));

	if (PropertyName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param 'property'"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 Index = FindEmitterHandleIndex(System, EmitterHandleId);
	if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter handle not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[Index].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("property"), PropertyName);

	// Aliased properties — mirror the aliases from HandleSetEmitterProperty
	if (PropertyName == TEXT("SimTarget") || PropertyName == TEXT("sim_target"))
	{
		R->SetStringField(TEXT("value"), ED->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("CPU") : TEXT("GPU"));
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("bLocalSpace") || PropertyName == TEXT("local_space"))
	{
		R->SetBoolField(TEXT("value"), ED->bLocalSpace);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("bDeterminism") || PropertyName == TEXT("determinism"))
	{
		R->SetBoolField(TEXT("value"), ED->bDeterminism);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("CalculateBoundsMode") || PropertyName == TEXT("calculate_bounds_mode") || PropertyName == TEXT("bounds_mode"))
	{
		FString ModeStr;
		switch (ED->CalculateBoundsMode)
		{
		case ENiagaraEmitterCalculateBoundMode::Dynamic: ModeStr = TEXT("Dynamic"); break;
		case ENiagaraEmitterCalculateBoundMode::Fixed: ModeStr = TEXT("Fixed"); break;
		case ENiagaraEmitterCalculateBoundMode::Programmable: ModeStr = TEXT("Programmable"); break;
		default: ModeStr = TEXT("Unknown"); break;
		}
		R->SetStringField(TEXT("value"), ModeStr);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("RandomSeed") || PropertyName == TEXT("random_seed"))
	{
		R->SetNumberField(TEXT("value"), ED->RandomSeed);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("AllocationMode") || PropertyName == TEXT("allocation_mode"))
	{
		FString ModeStr;
		switch (ED->AllocationMode)
		{
		case EParticleAllocationMode::AutomaticEstimate: ModeStr = TEXT("AutomaticEstimate"); break;
		case EParticleAllocationMode::ManualEstimate: ModeStr = TEXT("ManualEstimate"); break;
		case EParticleAllocationMode::FixedCount: ModeStr = TEXT("FixedCount"); break;
		default: ModeStr = TEXT("Unknown"); break;
		}
		R->SetStringField(TEXT("value"), ModeStr);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("PreAllocationCount") || PropertyName == TEXT("pre_allocation_count"))
	{
		R->SetNumberField(TEXT("value"), ED->PreAllocationCount);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("bRequiresPersistentIDs") || PropertyName == TEXT("requires_persistent_ids"))
	{
		R->SetBoolField(TEXT("value"), ED->bRequiresPersistentIDs);
		return NA_SuccessObj(R);
	}
	if (PropertyName == TEXT("MaxGPUParticlesSpawnPerFrame") || PropertyName == TEXT("max_gpu_particles_spawn_per_frame"))
	{
		R->SetNumberField(TEXT("value"), ED->MaxGPUParticlesSpawnPerFrame);
		return NA_SuccessObj(R);
	}

	// Generic fallback: use reflection on FVersionedNiagaraEmitterData
	FProperty* Prop = FVersionedNiagaraEmitterData::StaticStruct()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown emitter property '%s'. Use snake_case aliases (sim_target, local_space, determinism, bounds_mode, random_seed, allocation_mode, pre_allocation_count, requires_persistent_ids, max_gpu_particles_spawn_per_frame) or UProperty names. Use list_emitter_properties to see all available properties."),
			*PropertyName));
	}

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ED);

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
		Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
		R->SetStringField(TEXT("value"), ExportedValue);
	}

	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 6B: NPC Support (Task 24)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleCreateNPC(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	FString Namespace = Params->GetStringField(TEXT("namespace"));

	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: save_path"));
	if (Namespace.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: namespace"));

	// Validate the destination before the existing-asset probe or CreatePackage.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);
	FString FullPath = PackagePath / AssetName;

	// Check for existing asset — CreatePackage with same path returns existing in-memory package
	UNiagaraParameterCollection* Existing = LoadObject<UNiagaraParameterCollection>(nullptr, *FullPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("NPC already exists at '%s'"), *FullPath));
	}

	UPackage* Pkg = CreatePackage(*FullPath);
	if (!Pkg) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	UNiagaraParameterCollection* NPC = NewObject<UNiagaraParameterCollection>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!NPC) return FMonolithActionResult::Error(TEXT("Failed to create NPC"));

	// Set Namespace via FProperty reflection (it's protected but UPROPERTY EditAnywhere)
	FProperty* NsProp = UNiagaraParameterCollection::StaticClass()->FindPropertyByName(FName(TEXT("Namespace")));
	if (NsProp)
	{
		void* Addr = NsProp->ContainerPtrToValuePtr<void>(NPC);
		FName NsName(*Namespace);
		if (FNameProperty* NameProp = CastField<FNameProperty>(NsProp))
		{
			NameProp->SetPropertyValue(Addr, NsName);
		}
		else
		{
			NsProp->ImportText_Direct(*Namespace, Addr, NPC, PPF_None);
		}
	}
	else
	{
		UE_LOG(LogMonolithNiagara, Warning, TEXT("Could not find Namespace property on UNiagaraParameterCollection — namespace not set"));
	}

	NPC->RefreshCompileId();

	FAssetRegistryModule::AssetCreated(NPC);
	Pkg->MarkPackageDirty();

	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(Pkg, NPC, *PackageFilename, SaveArgs);
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("asset_path"), NPC->GetPathName());
	R->SetStringField(TEXT("namespace"), Namespace);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetNPC(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	UNiagaraParameterCollection* NPC = FMonolithAssetUtils::LoadAssetByPath<UNiagaraParameterCollection>(AssetPath);
	if (!NPC) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load NPC at '%s'"), *AssetPath));

	// Read namespace via reflection
	FString NamespaceStr;
	FProperty* NsProp = UNiagaraParameterCollection::StaticClass()->FindPropertyByName(FName(TEXT("Namespace")));
	if (NsProp)
	{
		const void* Addr = NsProp->ContainerPtrToValuePtr<void>(NPC);
		NsProp->ExportTextItem_Direct(NamespaceStr, Addr, nullptr, nullptr, PPF_None);
	}

	TArray<FNiagaraVariable> NPCParams = NPC->GetParameters();
	UNiagaraParameterCollectionInstance* DefaultInst = NPC->GetDefaultInstance();

	TArray<TSharedPtr<FJsonValue>> ParamArr;
	for (const FNiagaraVariable& Var : NPCParams)
	{
		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), Var.GetName().ToString());
		PO->SetStringField(TEXT("type"), Var.GetType().GetName());

		if (DefaultInst)
		{
			const FNiagaraParameterStore& Store = DefaultInst->GetParameterStore();
			PO->SetStringField(TEXT("default"), SerializeParameterValue(Var, Store));
		}

		ParamArr.Add(MakeShared<FJsonValueObject>(PO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), NPC->GetPathName());
	R->SetStringField(TEXT("namespace"), NamespaceStr);
	R->SetNumberField(TEXT("parameter_count"), NPCParams.Num());
	R->SetArrayField(TEXT("parameters"), ParamArr);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleAddNPCParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("name"));
	FString TypeName = Params->GetStringField(TEXT("type"));

	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	if (ParamName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: name"));
	if (TypeName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: type"));

	UNiagaraParameterCollection* NPC = FMonolithAssetUtils::LoadAssetByPath<UNiagaraParameterCollection>(AssetPath);
	if (!NPC) return FMonolithActionResult::Error(TEXT("Failed to load NPC"));

	FNiagaraTypeDefinition TD = ResolveNiagaraType(TypeName);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "AddNPCParam", "Add NPC Parameter"));
	NPC->Modify();

	int32 AddedIdx = NPC->AddParameter(FName(*ParamName), TD);

	NPC->RefreshCompileId();
	GEditor->EndTransaction();

	if (AddedIdx == INDEX_NONE) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add parameter '%s' — it may already exist"), *ParamName));

	NPC->GetPackage()->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("name"), ParamName);
	R->SetStringField(TEXT("type"), TD.GetName());
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveNPCParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("name"));

	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	if (ParamName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: name"));

	UNiagaraParameterCollection* NPC = FMonolithAssetUtils::LoadAssetByPath<UNiagaraParameterCollection>(AssetPath);
	if (!NPC) return FMonolithActionResult::Error(TEXT("Failed to load NPC"));

	// Find the parameter by name — NPC params may be stored with namespace prefix
	TArray<FNiagaraVariable> NPCParams = NPC->GetParameters();
	const FNiagaraVariable* Found = nullptr;
	for (const FNiagaraVariable& Var : NPCParams)
	{
		FString VarName = Var.GetName().ToString();
		if (VarName.Equals(ParamName, ESearchCase::IgnoreCase))
		{
			Found = &Var;
			break;
		}
	}
	if (!Found)
	{
		// Try suffix match for namespace-prefixed names (e.g. "NPC.Namespace.ParamName")
		for (const FNiagaraVariable& Var : NPCParams)
		{
			FString VarName = Var.GetName().ToString();
			if (VarName.EndsWith(ParamName))
			{
				Found = &Var;
				break;
			}
		}
	}
	if (!Found) return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter '%s' not found in NPC"), *ParamName));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemoveNPCParam", "Remove NPC Parameter"));
	NPC->Modify();

	NPC->RemoveParameter(*Found);
	NPC->RefreshCompileId();

	GEditor->EndTransaction();
	NPC->GetPackage()->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("removed"), Found->GetName().ToString());
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetNPCDefault(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	FString ParamName = Params->GetStringField(TEXT("name"));
	TSharedPtr<FJsonValue> ValueJV = Params->TryGetField(TEXT("value"));

	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	if (ParamName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: name"));
	if (!ValueJV.IsValid()) return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraParameterCollection* NPC = FMonolithAssetUtils::LoadAssetByPath<UNiagaraParameterCollection>(AssetPath);
	if (!NPC) return FMonolithActionResult::Error(TEXT("Failed to load NPC"));

	UNiagaraParameterCollectionInstance* DefaultInst = NPC->GetDefaultInstance();
	if (!DefaultInst) return FMonolithActionResult::Error(TEXT("Failed to get NPC default instance"));

	// Find the parameter — try exact match then suffix match
	TArray<FNiagaraVariable> NPCParams = NPC->GetParameters();
	const FNiagaraVariable* Found = nullptr;
	for (const FNiagaraVariable& Var : NPCParams)
	{
		FString VarName = Var.GetName().ToString();
		if (VarName.Equals(ParamName, ESearchCase::IgnoreCase) || VarName.EndsWith(ParamName))
		{
			Found = &Var;
			break;
		}
	}
	if (!Found) return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter '%s' not found in NPC"), *ParamName));

	const FNiagaraTypeDefinition& TD = Found->GetType();
	bool bSet = false;

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetNPCDefault", "Set NPC Default"));
	NPC->Modify();
	DefaultInst->Modify();

	// Use direct ParameterStore writes, NOT the Set*Parameter helpers.
	// Set*Parameter auto-prefixes with NPC namespace (e.g. "Speed" -> "NPC.TestNPC.Speed")
	// but AddParameter stores bare names. Direct store write matches the correct key.
	FNiagaraParameterStore& Store = DefaultInst->GetParameterStore();

	if (TD == FNiagaraTypeDefinition::GetFloatDef())
	{
		float FV = static_cast<float>(ValueJV->AsNumber());
		Store.SetParameterValue(FV, *Found);
		bSet = true;
	}
	else if (TD == FNiagaraTypeDefinition::GetIntDef())
	{
		int32 IV = static_cast<int32>(ValueJV->AsNumber());
		Store.SetParameterValue(IV, *Found);
		bSet = true;
	}
	else if (TD == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool BV; BV.SetValue(ValueJV->AsBool());
		Store.SetParameterValue(BV, *Found);
		bSet = true;
	}
	else if (TD == FNiagaraTypeDefinition::GetVec2Def())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(ValueJV);
		if (O.IsValid())
		{
			FVector2f V2(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")));
			Store.SetParameterValue(V2, *Found);
			bSet = true;
		}
	}
	else if (TD == FNiagaraTypeDefinition::GetVec3Def() || TD == FNiagaraTypeDefinition::GetPositionDef())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(ValueJV);
		if (O.IsValid())
		{
			FVector3f V3(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")));
			Store.SetParameterValue(V3, *Found);
			bSet = true;
		}
	}
	else if (TD == FNiagaraTypeDefinition::GetVec4Def())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(ValueJV);
		if (O.IsValid())
		{
			FVector4f V4(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")), O->GetNumberField(TEXT("w")));
			Store.SetParameterValue(V4, *Found);
			bSet = true;
		}
	}
	else if (TD == FNiagaraTypeDefinition::GetColorDef())
	{
		TSharedPtr<FJsonObject> O = AsObjectOrParseString(ValueJV);
		if (O.IsValid())
		{
			FLinearColor LC(
				static_cast<float>(O->GetNumberField(TEXT("r"))),
				static_cast<float>(O->GetNumberField(TEXT("g"))),
				static_cast<float>(O->GetNumberField(TEXT("b"))),
				O->HasField(TEXT("a")) ? static_cast<float>(O->GetNumberField(TEXT("a"))) : 1.0f);
			Store.SetParameterValue(LC, *Found);
			bSet = true;
		}
	}

	NPC->RefreshCompileId();
	GEditor->EndTransaction();

	if (!bSet) return FMonolithActionResult::Error(FString::Printf(TEXT("Unsupported type '%s' for set_npc_default"), *TD.GetName()));

	NPC->GetPackage()->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("parameter"), Found->GetName().ToString());
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 6B: Effect Type CRUD (Task 25)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleCreateEffectType(const TSharedPtr<FJsonObject>& Params)
{
	FString SavePath = Params->GetStringField(TEXT("save_path"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: save_path"));

	// Validate the destination before the existing-asset probe or CreatePackage.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	const FString PackagePath = FPackageName::GetLongPackagePath(SavePath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);
	FString FullPath = PackagePath / AssetName;

	// Check for existing asset
	UNiagaraEffectType* Existing = LoadObject<UNiagaraEffectType>(nullptr, *FullPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Effect type already exists at '%s'"), *FullPath));
	}

	UPackage* Pkg = CreatePackage(*FullPath);
	if (!Pkg) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	UNiagaraEffectType* ET = NewObject<UNiagaraEffectType>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!ET) return FMonolithActionResult::Error(TEXT("Failed to create effect type"));

	// Helper lambda: set enum property by name string via reflection
	auto SetEnumProp = [&](const TCHAR* PropName, const FString& ValueStr)
	{
		FProperty* Prop = UNiagaraEffectType::StaticClass()->FindPropertyByName(FName(PropName));
		if (!Prop) return;
		void* Addr = Prop->ContainerPtrToValuePtr<void>(ET);
		if (FEnumProperty* EP = CastField<FEnumProperty>(Prop))
		{
			UEnum* E = EP->GetEnum();
			if (E)
			{
				int64 EV = E->GetValueByNameString(ValueStr);
				if (EV != INDEX_NONE)
				{
					FNumericProperty* UP2 = EP->GetUnderlyingProperty();
					if (UP2) UP2->SetIntPropertyValue(Addr, EV);
				}
			}
		}
		else if (FByteProperty* ByP = CastField<FByteProperty>(Prop))
		{
			if (ByP->Enum)
			{
				int64 EV = ByP->Enum->GetValueByNameString(ValueStr);
				if (EV != INDEX_NONE) ByP->SetPropertyValue(Addr, static_cast<uint8>(EV));
			}
		}
		else
		{
			Prop->ImportText_Direct(*ValueStr, Addr, ET, PPF_None);
		}
	};

	if (Params->HasField(TEXT("cull_reaction")))
	{
		SetEnumProp(TEXT("CullReaction"), Params->GetStringField(TEXT("cull_reaction")));
	}

	if (Params->HasField(TEXT("update_frequency")))
	{
		SetEnumProp(TEXT("UpdateFrequency"), Params->GetStringField(TEXT("update_frequency")));
	}

	if (Params->HasField(TEXT("max_distance")))
	{
		float MaxDist = static_cast<float>(Params->GetNumberField(TEXT("max_distance")));
		// Try direct MaxDistance property first, then scalability settings
		FProperty* MaxDistProp = UNiagaraEffectType::StaticClass()->FindPropertyByName(FName(TEXT("MaxDistance")));
		if (MaxDistProp)
		{
			void* Addr = MaxDistProp->ContainerPtrToValuePtr<void>(ET);
			if (FFloatProperty* FP = CastField<FFloatProperty>(MaxDistProp))
				FP->SetPropertyValue(Addr, MaxDist);
			else if (FDoubleProperty* DP = CastField<FDoubleProperty>(MaxDistProp))
				DP->SetPropertyValue(Addr, static_cast<double>(MaxDist));
		}
	}

	FAssetRegistryModule::AssetCreated(ET);
	Pkg->MarkPackageDirty();

	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetName(), PackageFilename, FPackageName::GetAssetPackageExtension()))
	{
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		UPackage::SavePackage(Pkg, ET, *PackageFilename, SaveArgs);
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("asset_path"), ET->GetPathName());
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleGetEffectType(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	UNiagaraEffectType* ET = FMonolithAssetUtils::LoadAssetByPath<UNiagaraEffectType>(AssetPath);
	if (!ET) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load effect type at '%s'"), *AssetPath));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), ET->GetPathName());

	// Iterate all EditAnywhere UPROPERTYs via reflection
	TArray<TSharedPtr<FJsonValue>> PropsArr;
	for (TFieldIterator<FProperty> It(ET->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ET);

		if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
			PropObj->SetNumberField(TEXT("value"), FP->GetPropertyValue(ValuePtr));
		else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
			PropObj->SetNumberField(TEXT("value"), DP->GetPropertyValue(ValuePtr));
		else if (FIntProperty* IP = CastField<FIntProperty>(Prop))
			PropObj->SetNumberField(TEXT("value"), IP->GetPropertyValue(ValuePtr));
		else if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
			PropObj->SetBoolField(TEXT("value"), BP->GetPropertyValue(ValuePtr));
		else
		{
			FString ExportedValue;
			Prop->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, nullptr, PPF_None);
			PropObj->SetStringField(TEXT("value"), ExportedValue);
		}

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	R->SetArrayField(TEXT("properties"), PropsArr);
	return NA_SuccessObj(R);
}

FMonolithActionResult FMonolithNiagaraActions::HandleSetEffectTypeProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	FString PropertyName = Params->GetStringField(TEXT("property"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));

	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	if (PropertyName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: property"));
	if (!JV.IsValid()) return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	UNiagaraEffectType* ET = FMonolithAssetUtils::LoadAssetByPath<UNiagaraEffectType>(AssetPath);
	if (!ET) return FMonolithActionResult::Error(TEXT("Failed to load effect type"));

	FProperty* Prop = ET->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop) return FMonolithActionResult::Error(FString::Printf(TEXT("Property '%s' not found on UNiagaraEffectType"), *PropertyName));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetEffectTypeProp", "Set Effect Type Property"));
	ET->Modify();

	void* Addr = Prop->ContainerPtrToValuePtr<void>(ET);
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
		bOk = Prop->ImportText_Direct(*JV->AsString(), Addr, ET, PPF_None) != nullptr;
	}

	GEditor->EndTransaction();

	if (!bOk) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set property '%s'"), *PropertyName));

	ET->GetPackage()->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("property"), PropertyName);
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 6B: Parameter Discovery (Task 26)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetAvailableParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterFilter = Params->HasField(TEXT("emitter")) ? Params->GetStringField(TEXT("emitter")) : FString();
	FString UsageFilter = Params->HasField(TEXT("usage")) ? Params->GetStringField(TEXT("usage")).ToLower() : TEXT("all");

	if (SystemPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	TArray<TSharedPtr<FJsonValue>> All;

	// --- User Parameters ---
	if (UsageFilter == TEXT("all") || UsageFilter == TEXT("user"))
	{
		FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
		TArray<FNiagaraVariable> UP;
		US.GetUserParameters(UP);
		for (const FNiagaraVariable& P : UP)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), FString::Printf(TEXT("User.%s"), *P.GetName().ToString()));
			PO->SetStringField(TEXT("type"), P.GetType().GetName());
			PO->SetStringField(TEXT("scope"), TEXT("user"));
			All.Add(MakeShared<FJsonValueObject>(PO));
		}
	}

	// --- Engine Constants ---
	if (UsageFilter == TEXT("all") || UsageFilter == TEXT("engine"))
	{
		static const struct { const TCHAR* Name; const TCHAR* Type; } EngineParams[] = {
			{ TEXT("Engine.DeltaTime"), TEXT("float") },
			{ TEXT("Engine.InverseDeltaTime"), TEXT("float") },
			{ TEXT("Engine.ExecutionCount"), TEXT("int") },
			{ TEXT("Engine.Owner.Position"), TEXT("Vector") },
			{ TEXT("Engine.Owner.Velocity"), TEXT("Vector") },
			{ TEXT("Engine.Owner.Scale"), TEXT("Vector") },
			{ TEXT("Engine.Owner.XAxis"), TEXT("Vector") },
			{ TEXT("Engine.Owner.YAxis"), TEXT("Vector") },
			{ TEXT("Engine.Owner.ZAxis"), TEXT("Vector") },
			{ TEXT("Engine.Owner.SystemLocalToWorld"), TEXT("Matrix") },
			{ TEXT("Engine.Owner.SystemWorldToLocal"), TEXT("Matrix") },
			{ TEXT("Engine.Owner.SystemLocalToWorldNoScale"), TEXT("Matrix") },
			{ TEXT("Engine.Owner.Rotation"), TEXT("Quaternion") },
			{ TEXT("Engine.Owner.ExecutionState"), TEXT("int") },
			{ TEXT("Engine.GlobalSpawnCountScale"), TEXT("float") },
			{ TEXT("Engine.GlobalSystemCountScale"), TEXT("float") },
			{ TEXT("Engine.System.Age"), TEXT("float") },
			{ TEXT("Engine.System.TickCount"), TEXT("int") },
			{ TEXT("Engine.System.NumEmitters"), TEXT("int") },
			{ TEXT("Engine.System.NumEmittersAlive"), TEXT("int") },
			{ TEXT("Engine.System.SignificanceIndex"), TEXT("int") },
			{ TEXT("Engine.System.RandomSeed"), TEXT("int") },
			{ TEXT("Engine.Emitter.NumParticles"), TEXT("int") },
			{ TEXT("Engine.Emitter.TotalSpawnedParticles"), TEXT("int") },
			{ TEXT("Engine.Emitter.SpawnCountScale"), TEXT("float") },
			{ TEXT("Engine.Emitter.InstanceSeed"), TEXT("int") },
			{ TEXT("Engine.Emitter.LocalSpace"), TEXT("bool") },
			{ TEXT("Engine.Emitter.Determinism"), TEXT("bool") },
		};
		for (const auto& EP : EngineParams)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), EP.Name);
			PO->SetStringField(TEXT("type"), EP.Type);
			PO->SetStringField(TEXT("scope"), TEXT("engine"));
			All.Add(MakeShared<FJsonValueObject>(PO));
		}
	}

	// --- System Attributes ---
	if (UsageFilter == TEXT("all") || UsageFilter == TEXT("system"))
	{
		static const struct { const TCHAR* Name; const TCHAR* Type; } SystemAttrs[] = {
			{ TEXT("System.Age"), TEXT("float") },
			{ TEXT("System.CurrentLoopDuration"), TEXT("float") },
			{ TEXT("System.CurrentLoopDelay"), TEXT("float") },
			{ TEXT("System.ExecutionState"), TEXT("int") },
			{ TEXT("System.ExecutionStateSource"), TEXT("int") },
			{ TEXT("System.LoopCount"), TEXT("int") },
			{ TEXT("System.LoopedAge"), TEXT("float") },
			{ TEXT("System.NormalizedLoopAge"), TEXT("float") },
		};
		for (const auto& SA : SystemAttrs)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), SA.Name);
			PO->SetStringField(TEXT("type"), SA.Type);
			PO->SetStringField(TEXT("scope"), TEXT("system"));
			All.Add(MakeShared<FJsonValueObject>(PO));
		}
	}

	// --- Emitter Attributes ---
	if (UsageFilter == TEXT("all") || UsageFilter == TEXT("emitter"))
	{
		static const struct { const TCHAR* Name; const TCHAR* Type; } EmitterAttrs[] = {
			{ TEXT("Emitter.Age"), TEXT("float") },
			{ TEXT("Emitter.CurrentLoopDuration"), TEXT("float") },
			{ TEXT("Emitter.CurrentLoopDelay"), TEXT("float") },
			{ TEXT("Emitter.ExecutionState"), TEXT("int") },
			{ TEXT("Emitter.LoopCount"), TEXT("int") },
			{ TEXT("Emitter.LoopedAge"), TEXT("float") },
			{ TEXT("Emitter.NormalizedLoopAge"), TEXT("float") },
			{ TEXT("Emitter.LocalSpace"), TEXT("bool") },
			{ TEXT("Emitter.SpawnRate"), TEXT("float") },
			{ TEXT("Emitter.SpawnBurstInstantaneous"), TEXT("int") },
			{ TEXT("Emitter.SpawnGroup"), TEXT("int") },
			{ TEXT("Emitter.InterpSpawnStartDt"), TEXT("float") },
			{ TEXT("Emitter.RandomSeed"), TEXT("int") },
		};
		for (const auto& EA : EmitterAttrs)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), EA.Name);
			PO->SetStringField(TEXT("type"), EA.Type);
			PO->SetStringField(TEXT("scope"), TEXT("emitter"));
			All.Add(MakeShared<FJsonValueObject>(PO));
		}
	}

	// --- Particle Attributes ---
	if (UsageFilter == TEXT("all") || UsageFilter == TEXT("particle"))
	{
		// Well-known particle attributes
		static const struct { const TCHAR* Name; const TCHAR* Type; } WellKnown[] = {
			{ TEXT("Particles.Position"), TEXT("Vector") },
			{ TEXT("Particles.Velocity"), TEXT("Vector") },
			{ TEXT("Particles.Color"), TEXT("LinearColor") },
			{ TEXT("Particles.SpriteSize"), TEXT("Vector2D") },
			{ TEXT("Particles.SpriteRotation"), TEXT("float") },
			{ TEXT("Particles.NormalizedAge"), TEXT("float") },
			{ TEXT("Particles.Lifetime"), TEXT("float") },
			{ TEXT("Particles.Age"), TEXT("float") },
			{ TEXT("Particles.Mass"), TEXT("float") },
			{ TEXT("Particles.ID"), TEXT("int") },
			{ TEXT("Particles.UniqueID"), TEXT("int") },
			{ TEXT("Particles.MeshOrientation"), TEXT("Quaternion") },
			{ TEXT("Particles.Scale"), TEXT("Vector") },
			{ TEXT("Particles.SubImageIndex"), TEXT("float") },
			{ TEXT("Particles.DynamicMaterialParameter"), TEXT("Vector4") },
			{ TEXT("Particles.DynamicMaterialParameter1"), TEXT("Vector4") },
			{ TEXT("Particles.CameraOffset"), TEXT("float") },
			{ TEXT("Particles.RibbonWidth"), TEXT("float") },
			{ TEXT("Particles.RibbonLinkOrder"), TEXT("float") },
			{ TEXT("Particles.RibbonID"), TEXT("int") },
			{ TEXT("Particles.MaterialRandom"), TEXT("float") },
			{ TEXT("Particles.LightRadius"), TEXT("float") },
			{ TEXT("Particles.LightExponent"), TEXT("float") },
			{ TEXT("Particles.LightVolumetricScattering"), TEXT("float") },
		};
		for (const auto& WK : WellKnown)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), WK.Name);
			PO->SetStringField(TEXT("type"), WK.Type);
			PO->SetStringField(TEXT("scope"), TEXT("particle"));
			All.Add(MakeShared<FJsonValueObject>(PO));
		}

		// Try to read additional attributes from the compiled emitter dataset
		if (!EmitterFilter.IsEmpty())
		{
			int32 EIdx = FindEmitterHandleIndex(System, EmitterFilter);
			if (EIdx != INDEX_NONE)
			{
				FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
				if (ED)
				{
					for (ENiagaraScriptUsage U : { ENiagaraScriptUsage::ParticleSpawnScript, ENiagaraScriptUsage::ParticleUpdateScript })
					{
						UNiagaraScript* S = ED->GetScript(U, FGuid());
						if (!S) continue;

						auto RapidParams = S->RapidIterationParameters.ReadParameterVariables();
						for (const auto& RV : RapidParams)
						{
							FString VName = RV.GetName().ToString();
							if (VName.StartsWith(TEXT("Particles.")))
							{
								bool bDupe = false;
								for (const auto& E : All)
								{
									if (E->AsObject()->GetStringField(TEXT("name")) == VName)
									{
										bDupe = true;
										break;
									}
								}
								if (!bDupe)
								{
									TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
									PO->SetStringField(TEXT("name"), VName);
									PO->SetStringField(TEXT("type"), RV.GetType().GetName());
									PO->SetStringField(TEXT("scope"), TEXT("particle"));
									PO->SetBoolField(TEXT("from_compiled_data"), true);
									All.Add(MakeShared<FJsonValueObject>(PO));
								}
							}
						}
					}
				}
			}
		}
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), All.Num());
	R->SetArrayField(TEXT("parameters"), All);
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 6B: Preview System (Task 27)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandlePreviewSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	float SeekTime = Params->HasField(TEXT("seek_time")) ? static_cast<float>(Params->GetNumberField(TEXT("seek_time"))) : 1.0f;

	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TSharedPtr<FJsonValue>& ResVal = Params->TryGetField(TEXT("resolution"));
		if (ResVal->Type == EJson::Number)
		{
			// Single number = square resolution (e.g. resolution: 256)
			ResX = ResY = static_cast<int32>(ResVal->AsNumber());
		}
		else if (ResVal->Type == EJson::String)
		{
			FString ResStr = ResVal->AsString();
			int32 XPos;
			if (ResStr.FindChar('x', XPos) || ResStr.FindChar('X', XPos))
			{
				ResX = FCString::Atoi(*ResStr.Left(XPos));
				ResY = FCString::Atoi(*ResStr.Mid(XPos + 1));
			}
			else
			{
				// Plain numeric string (e.g. resolution: "256")
				ResX = ResY = FCString::Atoi(*ResStr);
			}
		}
		else if (ResVal->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = ResVal->AsArray();
			if (Arr.Num() >= 2)
			{
				ResX = static_cast<int32>(Arr[0]->AsNumber());
				ResY = static_cast<int32>(Arr[1]->AsNumber());
			}
		}
		if (ResX <= 0) ResX = 512;
		if (ResY <= 0) ResY = 512;
	}

	FString OutputPath;
	if (Params->HasField(TEXT("output_path")))
	{
		OutputPath = Params->GetStringField(TEXT("output_path"));
	}

	// Camera angle presets
	FVector CameraLocation(200.0f, 200.0f, 150.0f);
	FRotator CameraRotation(-20.0f, -135.0f, 0.0f);
	float FOV = 60.0f;

	FString CameraAngle = Params->HasField(TEXT("camera_angle")) ? Params->GetStringField(TEXT("camera_angle")).ToLower() : TEXT("three_quarter");
	if (CameraAngle == TEXT("front"))
	{
		CameraLocation = FVector(300.0f, 0.0f, 50.0f);
		CameraRotation = FRotator(-5.0f, 180.0f, 0.0f);
	}
	else if (CameraAngle == TEXT("top"))
	{
		CameraLocation = FVector(0.0f, 0.0f, 400.0f);
		CameraRotation = FRotator(-90.0f, 0.0f, 0.0f);
	}
	else if (CameraAngle == TEXT("side"))
	{
		CameraLocation = FVector(0.0f, 300.0f, 50.0f);
		CameraRotation = FRotator(-5.0f, -90.0f, 0.0f);
	}
	// else "three_quarter" — defaults above

	// Route through the tool registry to editor_query("capture_scene_preview")
	// No compile-time dependency on MonolithEditor — pure runtime dispatch
	TSharedRef<FJsonObject> EditorParams = MakeShared<FJsonObject>();
	EditorParams->SetStringField(TEXT("asset_path"), AssetPath);
	EditorParams->SetStringField(TEXT("asset_type"), TEXT("niagara"));
	EditorParams->SetNumberField(TEXT("seek_time"), SeekTime);

	TArray<TSharedPtr<FJsonValue>> ResArr;
	ResArr.Add(MakeShared<FJsonValueNumber>(ResX));
	ResArr.Add(MakeShared<FJsonValueNumber>(ResY));
	EditorParams->SetArrayField(TEXT("resolution"), ResArr);

	if (!OutputPath.IsEmpty())
	{
		EditorParams->SetStringField(TEXT("output_path"), OutputPath);
	}

	// Build camera object
	TSharedRef<FJsonObject> CameraObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(CameraLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(CameraLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(CameraLocation.Z));
	CameraObj->SetArrayField(TEXT("location"), LocArr);
	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(CameraRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(CameraRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(CameraRotation.Roll));
	CameraObj->SetArrayField(TEXT("rotation"), RotArr);
	CameraObj->SetNumberField(TEXT("fov"), FOV);
	EditorParams->SetObjectField(TEXT("camera"), CameraObj);

	// Pass through background_color if provided
	if (Params->HasField(TEXT("background_color")))
	{
		EditorParams->SetField(TEXT("background_color"), Params->TryGetField(TEXT("background_color")));
	}

	FMonolithActionResult EditorResult = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("editor"), TEXT("capture_scene_preview"), EditorParams);

	if (!EditorResult.bSuccess)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Preview capture failed: %s. Ensure the MonolithEditor module is loaded."),
			*EditorResult.ErrorMessage));
	}

	// Inject camera_angle into response so callers know which angle was used
	if (EditorResult.bSuccess && EditorResult.Result.IsValid())
	{
		EditorResult.Result->SetStringField(TEXT("camera_angle"), CameraAngle);
	}

	return EditorResult;
}

// ============================================================================
// Phase 6A: Event Handlers, Simulation Stages, Module Outputs (7 new actions)
// ============================================================================

// ============================================================================
// Action: get_event_handlers
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetEventHandlers(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const TArray<FNiagaraEventScriptProperties>& Handlers = ED->GetEventHandlers();

	TArray<TSharedPtr<FJsonValue>> HandlersArr;
	for (int32 i = 0; i < Handlers.Num(); ++i)
	{
		const FNiagaraEventScriptProperties& ESP = Handlers[i];
		TSharedRef<FJsonObject> HO = MakeShared<FJsonObject>();
		HO->SetNumberField(TEXT("index"), i);
		HO->SetStringField(TEXT("source_event_name"), ESP.SourceEventName.ToString());
		HO->SetStringField(TEXT("source_emitter_id"), ESP.SourceEmitterID.ToString());

		// Resolve source emitter name if possible
		if (ESP.SourceEmitterID.IsValid())
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetId() == ESP.SourceEmitterID)
				{
					HO->SetStringField(TEXT("source_emitter_name"), Handle.GetName().ToString());
					break;
				}
			}
		}

		// Execution mode
		FString ExecModeStr;
		switch (ESP.ExecutionMode)
		{
		case EScriptExecutionMode::EveryParticle: ExecModeStr = TEXT("EveryParticle"); break;
		case EScriptExecutionMode::SpawnedParticles: ExecModeStr = TEXT("SpawnedParticles"); break;
		case EScriptExecutionMode::SingleParticle: ExecModeStr = TEXT("SingleParticle"); break;
		default: ExecModeStr = TEXT("Unknown"); break;
		}
		HO->SetStringField(TEXT("execution_mode"), ExecModeStr);

		HO->SetNumberField(TEXT("spawn_number"), static_cast<double>(ESP.SpawnNumber));
		HO->SetNumberField(TEXT("max_events_per_frame"), static_cast<double>(ESP.MaxEventsPerFrame));
		HO->SetBoolField(TEXT("random_spawn_number"), ESP.bRandomSpawnNumber);
		HO->SetNumberField(TEXT("min_spawn_number"), static_cast<double>(ESP.MinSpawnNumber));
		HO->SetBoolField(TEXT("update_attribute_initial_values"), ESP.UpdateAttributeInitialValues);

		// Script usage ID
		if (ESP.Script)
		{
			HO->SetStringField(TEXT("usage_id"), ESP.Script->GetUsageId().ToString());
		}

		HandlersArr.Add(MakeShared<FJsonValueObject>(HO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Handlers.Num());
	R->SetArrayField(TEXT("event_handlers"), HandlersArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: set_event_handler_property
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSetEventHandlerProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString PropertyName = Params->GetStringField(TEXT("property"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid()) return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	int32 HandlerIndex = Params->HasField(TEXT("handler_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("handler_index"))) : -1;
	FString UsageIdStr = Params->HasField(TEXT("usage_id")) ? Params->GetStringField(TEXT("usage_id")) : FString();

	if (HandlerIndex < 0 && UsageIdStr.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Must provide handler_index or usage_id"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	// Find the handler
	FNiagaraEventScriptProperties* Handler = nullptr;
	if (!UsageIdStr.IsEmpty())
	{
		FGuid UsageId;
		if (!FGuid::Parse(UsageIdStr, UsageId))
			return FMonolithActionResult::Error(TEXT("Invalid usage_id GUID"));
		Handler = ED->GetEventHandlerByIdUnsafe(UsageId);
		if (!Handler)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Event handler with usage_id '%s' not found"), *UsageIdStr));
	}
	else
	{
		const TArray<FNiagaraEventScriptProperties>& Handlers = ED->GetEventHandlers();
		if (!Handlers.IsValidIndex(HandlerIndex))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Handler index %d out of range (0-%d)"), HandlerIndex, Handlers.Num() - 1));
		// GetEventHandlers returns const ref — use GetEventHandlerByIdUnsafe with the script's usage ID
		if (Handlers[HandlerIndex].Script)
		{
			Handler = ED->GetEventHandlerByIdUnsafe(Handlers[HandlerIndex].Script->GetUsageId());
		}
		if (!Handler)
			return FMonolithActionResult::Error(TEXT("Failed to get mutable event handler pointer"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetEvtHandlerProp", "Set Event Handler Property"));
	System->Modify();

	FString ValueStr = JV->Type == EJson::String ? JV->AsString() : FString();
	double ValueNum = JV->Type == EJson::Number ? JV->AsNumber() : 0.0;
	bool ValueBool = JV->Type == EJson::Boolean ? JV->AsBool() : false;

	bool bSet = false;

	if (PropertyName == TEXT("ExecutionMode") || PropertyName == TEXT("execution_mode"))
	{
		FString Mode = ValueStr.ToLower();
		if (Mode == TEXT("everyparticle") || Mode == TEXT("every_particle")) Handler->ExecutionMode = EScriptExecutionMode::EveryParticle;
		else if (Mode == TEXT("spawnedparticles") || Mode == TEXT("spawned_particles")) Handler->ExecutionMode = EScriptExecutionMode::SpawnedParticles;
		else if (Mode == TEXT("singleparticle") || Mode == TEXT("single_particle")) Handler->ExecutionMode = EScriptExecutionMode::SingleParticle;
		else { GEditor->EndTransaction(); return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown execution mode '%s'"), *ValueStr)); }
		bSet = true;
	}
	else if (PropertyName == TEXT("SpawnNumber") || PropertyName == TEXT("spawn_number"))
	{
		Handler->SpawnNumber = static_cast<uint32>(JV->Type == EJson::Number ? ValueNum : FCString::Atoi(*ValueStr));
		bSet = true;
	}
	else if (PropertyName == TEXT("MaxEventsPerFrame") || PropertyName == TEXT("max_events_per_frame"))
	{
		Handler->MaxEventsPerFrame = static_cast<uint32>(JV->Type == EJson::Number ? ValueNum : FCString::Atoi(*ValueStr));
		bSet = true;
	}
	else if (PropertyName == TEXT("SourceEventName") || PropertyName == TEXT("source_event_name"))
	{
		Handler->SourceEventName = FName(*ValueStr);
		bSet = true;
	}
	else if (PropertyName == TEXT("bRandomSpawnNumber") || PropertyName == TEXT("random_spawn_number"))
	{
		Handler->bRandomSpawnNumber = JV->Type == EJson::Boolean ? ValueBool : (ValueStr.ToBool());
		bSet = true;
	}
	else if (PropertyName == TEXT("MinSpawnNumber") || PropertyName == TEXT("min_spawn_number"))
	{
		Handler->MinSpawnNumber = static_cast<uint32>(JV->Type == EJson::Number ? ValueNum : FCString::Atoi(*ValueStr));
		bSet = true;
	}
	else if (PropertyName == TEXT("UpdateAttributeInitialValues") || PropertyName == TEXT("update_attribute_initial_values"))
	{
		Handler->UpdateAttributeInitialValues = JV->Type == EJson::Boolean ? ValueBool : (ValueStr.ToBool());
		bSet = true;
	}
	else if (PropertyName == TEXT("SourceEmitterID") || PropertyName == TEXT("source_emitter_id") || PropertyName == TEXT("source_emitter"))
	{
		// Accept emitter name/handle ID or GUID directly
		FGuid NewSourceGuid;
		if (FGuid::Parse(ValueStr, NewSourceGuid))
		{
			Handler->SourceEmitterID = NewSourceGuid;
		}
		else
		{
			// Resolve as emitter name/handle ID
			int32 SrcIdx = FindEmitterHandleIndex(System, ValueStr);
			if (SrcIdx == INDEX_NONE) { GEditor->EndTransaction(); return FMonolithActionResult::Error(FString::Printf(TEXT("Source emitter '%s' not found"), *ValueStr)); }
			Handler->SourceEmitterID = System->GetEmitterHandles()[SrcIdx].GetId();
		}
		bSet = true;
	}

	GEditor->EndTransaction();

	if (!bSet)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown event handler property '%s'. Valid: ExecutionMode, SpawnNumber, MaxEventsPerFrame, SourceEventName, bRandomSpawnNumber, MinSpawnNumber, UpdateAttributeInitialValues, SourceEmitterID"), *PropertyName));

	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("property"), PropertyName);
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: remove_event_handler
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveEventHandler(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	int32 HandlerIndex = Params->HasField(TEXT("handler_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("handler_index"))) : -1;
	FString UsageIdStr = Params->HasField(TEXT("usage_id")) ? Params->GetStringField(TEXT("usage_id")) : FString();

	if (HandlerIndex < 0 && UsageIdStr.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Must provide handler_index or usage_id"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FNiagaraEmitterHandle& EmitterHandle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitterData* ED = EmitterHandle.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	FVersionedNiagaraEmitter VersionedEmitter = EmitterHandle.GetInstance();
	UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
	if (!Emitter) return FMonolithActionResult::Error(TEXT("Emitter instance is null"));

	// Resolve usage ID
	FGuid UsageId;
	if (!UsageIdStr.IsEmpty())
	{
		if (!FGuid::Parse(UsageIdStr, UsageId))
			return FMonolithActionResult::Error(TEXT("Invalid usage_id GUID"));
	}
	else
	{
		const TArray<FNiagaraEventScriptProperties>& Handlers = ED->GetEventHandlers();
		if (!Handlers.IsValidIndex(HandlerIndex))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Handler index %d out of range (0-%d)"), HandlerIndex, Handlers.Num() - 1));
		if (Handlers[HandlerIndex].Script)
			UsageId = Handlers[HandlerIndex].Script->GetUsageId();
		else
			return FMonolithActionResult::Error(TEXT("Handler has no script — cannot determine usage ID"));
	}

	FString RemovedEventName;
	{
		FNiagaraEventScriptProperties* HandlerPtr = ED->GetEventHandlerByIdUnsafe(UsageId);
		if (HandlerPtr)
			RemovedEventName = HandlerPtr->SourceEventName.ToString();
		else
			return FMonolithActionResult::Error(TEXT("Event handler not found by usage_id"));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemoveEvtHandler", "Remove Event Handler"));
	System->Modify();

	Emitter->RemoveEventHandlerByUsageId(UsageId, VersionedEmitter.Version);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("removed_event"), RemovedEventName);
	R->SetNumberField(TEXT("remaining_handlers"), ED->GetEventHandlers().Num());
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: get_simulation_stages
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetSimulationStages(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();

	TArray<TSharedPtr<FJsonValue>> StagesArr;
	for (int32 i = 0; i < Stages.Num(); ++i)
	{
		UNiagaraSimulationStageBase* StageBase = Stages[i];
		if (!StageBase) continue;

		TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
		SO->SetNumberField(TEXT("index"), i);
		SO->SetStringField(TEXT("name"), StageBase->SimulationStageName.ToString());
		SO->SetBoolField(TEXT("enabled"), StageBase->bEnabled != 0);
		SO->SetStringField(TEXT("class"), StageBase->GetClass()->GetName());

		if (StageBase->Script)
		{
			SO->SetStringField(TEXT("usage_id"), StageBase->Script->GetUsageId().ToString());
		}
		SO->SetStringField(TEXT("merge_id"), StageBase->GetMergeId().ToString());

		// Generic-specific properties
		if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(StageBase))
		{
			FString IterStr;
			switch (Generic->IterationSource)
			{
			case ENiagaraIterationSource::Particles: IterStr = TEXT("Particles"); break;
			case ENiagaraIterationSource::DataInterface: IterStr = TEXT("DataInterface"); break;
			case ENiagaraIterationSource::DirectSet: IterStr = TEXT("DirectSet"); break;
			default: IterStr = TEXT("Unknown"); break;
			}
			SO->SetStringField(TEXT("iteration_source"), IterStr);

			FString ExecBehaviorStr;
			switch (Generic->ExecuteBehavior)
			{
			case ENiagaraSimStageExecuteBehavior::Always: ExecBehaviorStr = TEXT("Always"); break;
			case ENiagaraSimStageExecuteBehavior::OnSimulationReset: ExecBehaviorStr = TEXT("OnSimulationReset"); break;
			case ENiagaraSimStageExecuteBehavior::NotOnSimulationReset: ExecBehaviorStr = TEXT("NotOnSimulationReset"); break;
			default: ExecBehaviorStr = TEXT("Unknown"); break;
			}
			SO->SetStringField(TEXT("execute_behavior"), ExecBehaviorStr);

			SO->SetBoolField(TEXT("disable_partial_particle_update"), Generic->bDisablePartialParticleUpdate != 0);
			SO->SetBoolField(TEXT("particle_iteration_state_enabled"), Generic->bParticleIterationStateEnabled != 0);
			SO->SetBoolField(TEXT("gpu_dispatch_force_linear"), Generic->bGpuDispatchForceLinear != 0);

			if (Generic->IterationSource == ENiagaraIterationSource::DataInterface)
			{
				SO->SetStringField(TEXT("data_interface"), Generic->DataInterface.BoundVariable.GetName().ToString());
			}

			// Reflection-based remaining UPROPERTYs
			TSharedRef<FJsonObject> PropsObj = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(UNiagaraSimulationStageGeneric::StaticClass()); It; ++It)
			{
				FProperty* Prop = *It;
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
				FString PropName = Prop->GetName();
				if (PropName == TEXT("SimulationStageName") || PropName == TEXT("bEnabled")
					|| PropName == TEXT("IterationSource") || PropName == TEXT("ExecuteBehavior")
					|| PropName == TEXT("bDisablePartialParticleUpdate") || PropName == TEXT("bParticleIterationStateEnabled")
					|| PropName == TEXT("bGpuDispatchForceLinear") || PropName == TEXT("DataInterface"))
					continue;
				FString ValStr;
				Prop->ExportTextItem_Direct(ValStr, Prop->ContainerPtrToValuePtr<void>(Generic), nullptr, Generic, PPF_None);
				if (ValStr.Len() <= 200)
					PropsObj->SetStringField(PropName, ValStr);
			}
			if (PropsObj->Values.Num() > 0)
				SO->SetObjectField(TEXT("properties"), PropsObj);
		}

		StagesArr.Add(MakeShared<FJsonValueObject>(SO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Stages.Num());
	R->SetArrayField(TEXT("simulation_stages"), StagesArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: set_simulation_stage_property
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSetSimulationStageProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString PropertyName = Params->GetStringField(TEXT("property"));
	TSharedPtr<FJsonValue> JV = Params->TryGetField(TEXT("value"));
	if (!JV.IsValid()) return FMonolithActionResult::Error(TEXT("Missing required field: value"));

	int32 StageIndex = Params->HasField(TEXT("stage_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("stage_index"))) : -1;
	FString StageName = Params->HasField(TEXT("stage_name")) ? Params->GetStringField(TEXT("stage_name")) : FString();

	if (StageIndex < 0 && StageName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Must provide stage_index or stage_name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FVersionedNiagaraEmitterData* ED = System->GetEmitterHandles()[EIdx].GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();

	UNiagaraSimulationStageBase* TargetStage = nullptr;
	if (StageIndex >= 0)
	{
		if (!Stages.IsValidIndex(StageIndex))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Stage index %d out of range (0-%d)"), StageIndex, Stages.Num() - 1));
		TargetStage = Stages[StageIndex];
	}
	else
	{
		FName TargetName(*StageName);
		for (UNiagaraSimulationStageBase* S : Stages)
		{
			if (S && S->SimulationStageName == TargetName)
			{
				TargetStage = S;
				break;
			}
		}
		if (!TargetStage)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Simulation stage '%s' not found"), *StageName));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetSimStageProp", "Set Simulation Stage Property"));
	System->Modify();
	TargetStage->Modify();

	FString ValueStr = JV->Type == EJson::String ? JV->AsString() : FString();

	bool bSet = false;

	if (PropertyName == TEXT("SimulationStageName") || PropertyName == TEXT("name"))
	{
		TargetStage->SimulationStageName = FName(*ValueStr);
		bSet = true;
	}
	else if (PropertyName == TEXT("bEnabled") || PropertyName == TEXT("enabled"))
	{
		bool bVal = JV->Type == EJson::Boolean ? JV->AsBool() : ValueStr.ToBool();
		TargetStage->bEnabled = bVal ? 1 : 0;
		bSet = true;
	}
	else if (UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(TargetStage))
	{
		if (PropertyName == TEXT("IterationSource") || PropertyName == TEXT("iteration_source"))
		{
			FString Lower = ValueStr.ToLower();
			if (Lower == TEXT("particles")) Generic->IterationSource = ENiagaraIterationSource::Particles;
			else if (Lower == TEXT("datainterface") || Lower == TEXT("data_interface")) Generic->IterationSource = ENiagaraIterationSource::DataInterface;
			else if (Lower == TEXT("directset") || Lower == TEXT("direct_set")) Generic->IterationSource = ENiagaraIterationSource::DirectSet;
			else { GEditor->EndTransaction(); return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown iteration source '%s'. Valid: Particles, DataInterface, DirectSet"), *ValueStr)); }
			bSet = true;
		}
		else if (PropertyName == TEXT("ExecuteBehavior") || PropertyName == TEXT("execute_behavior"))
		{
			FString Lower = ValueStr.ToLower();
			if (Lower == TEXT("always")) Generic->ExecuteBehavior = ENiagaraSimStageExecuteBehavior::Always;
			else if (Lower == TEXT("onsimulationreset") || Lower == TEXT("on_simulation_reset")) Generic->ExecuteBehavior = ENiagaraSimStageExecuteBehavior::OnSimulationReset;
			else if (Lower == TEXT("notonsimulationreset") || Lower == TEXT("not_on_simulation_reset")) Generic->ExecuteBehavior = ENiagaraSimStageExecuteBehavior::NotOnSimulationReset;
			else { GEditor->EndTransaction(); return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown execute behavior '%s'"), *ValueStr)); }
			bSet = true;
		}
		else if (PropertyName == TEXT("bDisablePartialParticleUpdate") || PropertyName == TEXT("disable_partial_particle_update"))
		{
			bool bVal = JV->Type == EJson::Boolean ? JV->AsBool() : ValueStr.ToBool();
			Generic->bDisablePartialParticleUpdate = bVal ? 1 : 0;
			bSet = true;
		}
		else if (PropertyName == TEXT("bGpuDispatchForceLinear") || PropertyName == TEXT("gpu_dispatch_force_linear"))
		{
			bool bVal = JV->Type == EJson::Boolean ? JV->AsBool() : ValueStr.ToBool();
			Generic->bGpuDispatchForceLinear = bVal ? 1 : 0;
			bSet = true;
		}
		else if (PropertyName == TEXT("bParticleIterationStateEnabled") || PropertyName == TEXT("particle_iteration_state_enabled"))
		{
			bool bVal = JV->Type == EJson::Boolean ? JV->AsBool() : ValueStr.ToBool();
			Generic->bParticleIterationStateEnabled = bVal ? 1 : 0;
			bSet = true;
		}

		// Reflection fallback for any UNiagaraSimulationStageGeneric UPROPERTY
		if (!bSet)
		{
			FProperty* Prop = Generic->GetClass()->FindPropertyByName(FName(*PropertyName));
			if (Prop)
			{
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Generic);
				if (FFloatProperty* FP = CastField<FFloatProperty>(Prop))
				{
					FP->SetPropertyValue(ValuePtr, JV->Type == EJson::Number ? JV->AsNumber() : FCString::Atof(*ValueStr));
					bSet = true;
				}
				else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
				{
					DP->SetPropertyValue(ValuePtr, JV->Type == EJson::Number ? JV->AsNumber() : FCString::Atod(*ValueStr));
					bSet = true;
				}
				else if (FIntProperty* IP = CastField<FIntProperty>(Prop))
				{
					IP->SetPropertyValue(ValuePtr, JV->Type == EJson::Number ? static_cast<int32>(JV->AsNumber()) : FCString::Atoi(*ValueStr));
					bSet = true;
				}
				else if (FBoolProperty* BP = CastField<FBoolProperty>(Prop))
				{
					BP->SetPropertyValue(ValuePtr, JV->Type == EJson::Boolean ? JV->AsBool() : ValueStr.ToBool());
					bSet = true;
				}
				else
				{
					if (ValueStr.IsEmpty() && JV->Type == EJson::Number)
						ValueStr = FString::Printf(TEXT("%g"), JV->AsNumber());
					const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueStr, ValuePtr, Generic, PPF_None);
					bSet = (ImportResult != nullptr);
				}
			}
		}
	}

	GEditor->EndTransaction();

	if (!bSet)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown simulation stage property '%s'. Use UProperty names or snake_case aliases: name, enabled, iteration_source, execute_behavior, disable_partial_particle_update, gpu_dispatch_force_linear, particle_iteration_state_enabled"),
			*PropertyName));

	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("property"), PropertyName);
	R->SetStringField(TEXT("stage_name"), TargetStage->SimulationStageName.ToString());
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: remove_simulation_stage
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleRemoveSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	int32 StageIndex = Params->HasField(TEXT("stage_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("stage_index"))) : -1;
	FString StageName = Params->HasField(TEXT("stage_name")) ? Params->GetStringField(TEXT("stage_name")) : FString();

	if (StageIndex < 0 && StageName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Must provide stage_index or stage_name"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	FNiagaraEmitterHandle& EmitterHandle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitterData* ED = EmitterHandle.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("No emitter data"));

	FVersionedNiagaraEmitter VersionedEmitter = EmitterHandle.GetInstance();
	UNiagaraEmitter* Emitter = VersionedEmitter.Emitter;
	if (!Emitter) return FMonolithActionResult::Error(TEXT("Emitter instance is null"));

	const TArray<UNiagaraSimulationStageBase*>& Stages = ED->GetSimulationStages();

	UNiagaraSimulationStageBase* TargetStage = nullptr;
	if (StageIndex >= 0)
	{
		if (!Stages.IsValidIndex(StageIndex))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Stage index %d out of range (0-%d)"), StageIndex, Stages.Num() - 1));
		TargetStage = Stages[StageIndex];
	}
	else
	{
		FName TargetName(*StageName);
		for (UNiagaraSimulationStageBase* S : Stages)
		{
			if (S && S->SimulationStageName == TargetName)
			{
				TargetStage = S;
				break;
			}
		}
		if (!TargetStage)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Simulation stage '%s' not found"), *StageName));
	}

	FString RemovedName = TargetStage->SimulationStageName.ToString();

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RemoveSimStage", "Remove Simulation Stage"));
	System->Modify();

	Emitter->RemoveSimulationStage(TargetStage, VersionedEmitter.Version);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("removed_stage"), RemovedName);
	R->SetNumberField(TEXT("remaining_stages"), ED->GetSimulationStages().Num());
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: get_module_output_parameters
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleOutputParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	// GetStackFunctionOutputVariables is NOT exported (no NIAGARAEDITOR_API).
	// Alternative: inspect the module's script graph for output variables via the output node,
	// and also check which Particles.* attributes appear in the module's ParameterMap output pins.
	TArray<TSharedPtr<FJsonValue>> OutputsArr;

	// Approach: load the module's script and inspect its output node
	if (UNiagaraScript* FuncScript = MN->FunctionScript.Get())
	{
		if (UNiagaraScriptSource* ScriptSrc = Cast<UNiagaraScriptSource>(FuncScript->GetLatestSource()))
		{
			if (UNiagaraGraph* FuncGraph = ScriptSrc->NodeGraph)
			{
				TArray<UNiagaraNodeOutput*> OutputNodes;
				FuncGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
				for (UNiagaraNodeOutput* OutNode : OutputNodes)
				{
					for (const FNiagaraVariable& OutVar : OutNode->GetOutputs())
					{
						TSharedRef<FJsonObject> VO = MakeShared<FJsonObject>();
						VO->SetStringField(TEXT("name"), OutVar.GetName().ToString());
						VO->SetStringField(TEXT("type"), OutVar.GetType().GetName());
						OutputsArr.Add(MakeShared<FJsonValueObject>(VO));
					}
				}
			}
		}
	}

	// Also check the module's ParameterMap output for written attributes
	// (attributes written via ParameterMapSet that aren't in the formal output list)
	UEdGraphPin* MapOut = MonolithNiagaraHelpers::GetParameterMapPin(*MN, EGPD_Output);
	if (MapOut)
	{
		// The module's output ParameterMap contains all attributes it writes.
		// We can inspect the pins on the override node downstream to see what was written.
		// But the formal output node approach above covers the standard case.
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("module"), MN->GetFunctionName());
	R->SetStringField(TEXT("module_node"), MN->NodeGuid.ToString());
	R->SetNumberField(TEXT("output_count"), OutputsArr.Num());
	R->SetArrayField(TEXT("outputs"), OutputsArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 7: Advanced Features
// ============================================================================

// ============================================================================
// Action: diff_systems (Task 29)
// ============================================================================

// Helper: export a system's spec as JSON for diff comparison
static TSharedPtr<FJsonObject> ExportSpecForDiff(UNiagaraSystem* System)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), System->GetPathName());
	Params->SetBoolField(TEXT("include_values"), true);
	FMonolithActionResult SpecResult = FMonolithNiagaraActions::HandleExportSystemSpec(Params);
	if (!SpecResult.bSuccess || !SpecResult.Result.IsValid()) return nullptr;
	return SpecResult.Result->GetObjectField(TEXT("spec"));
}

// Helper: compare two JSON objects, return fields that differ
static TSharedRef<FJsonObject> DiffJsonObjects(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B,
	const FString& Context)
{
	TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>();
	if (!A.IsValid() && !B.IsValid()) return Diff;
	if (!A.IsValid() || !B.IsValid())
	{
		Diff->SetStringField(TEXT("status"), !A.IsValid() ? TEXT("added") : TEXT("removed"));
		return Diff;
	}

	// Collect all keys from both
	TSet<FString> AllKeys;
	for (auto& Pair : A->Values) AllKeys.Add(MonolithKeyToString(Pair.Key));
	for (auto& Pair : B->Values) AllKeys.Add(MonolithKeyToString(Pair.Key));

	TArray<TSharedPtr<FJsonValue>> Changes;
	for (const FString& Key : AllKeys)
	{
		TSharedPtr<FJsonValue> VA = A->TryGetField(Key);
		TSharedPtr<FJsonValue> VB = B->TryGetField(Key);

		if (!VA.IsValid())
		{
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("field"), Key);
			C->SetStringField(TEXT("change"), TEXT("added_in_b"));
			Changes.Add(MakeShared<FJsonValueObject>(C));
		}
		else if (!VB.IsValid())
		{
			TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("field"), Key);
			C->SetStringField(TEXT("change"), TEXT("removed_in_b"));
			Changes.Add(MakeShared<FJsonValueObject>(C));
		}
		else
		{
			// Compare as strings for simplicity
			FString SA, SB;
			if (VA->Type == EJson::String) SA = VA->AsString();
			else if (VA->Type == EJson::Number) SA = FString::Printf(TEXT("%g"), VA->AsNumber());
			else if (VA->Type == EJson::Boolean) SA = VA->AsBool() ? TEXT("true") : TEXT("false");

			if (VB->Type == EJson::String) SB = VB->AsString();
			else if (VB->Type == EJson::Number) SB = FString::Printf(TEXT("%g"), VB->AsNumber());
			else if (VB->Type == EJson::Boolean) SB = VB->AsBool() ? TEXT("true") : TEXT("false");

			if (!SA.IsEmpty() && !SB.IsEmpty() && SA != SB)
			{
				TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("field"), Key);
				C->SetStringField(TEXT("a"), SA);
				C->SetStringField(TEXT("b"), SB);
				Changes.Add(MakeShared<FJsonValueObject>(C));
			}
		}
	}
	if (Changes.Num() > 0)
		Diff->SetArrayField(TEXT("changes"), Changes);
	return Diff;
}

FMonolithActionResult FMonolithNiagaraActions::HandleDiffSystems(const TSharedPtr<FJsonObject>& Params)
{
	FString PathA = Params->GetStringField(TEXT("asset_path_a"));
	FString PathB = Params->GetStringField(TEXT("asset_path_b"));
	FString DetailLevel = Params->HasField(TEXT("detail_level")) ? Params->GetStringField(TEXT("detail_level")).ToLower() : TEXT("full");
	bool bSummary = (DetailLevel == TEXT("summary"));

	if (PathA.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path_a"));
	if (PathB.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path_b"));

	UNiagaraSystem* SysA = LoadSystem(PathA);
	if (!SysA) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system A: %s"), *PathA));
	UNiagaraSystem* SysB = LoadSystem(PathB);
	if (!SysB) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system B: %s"), *PathB));

	TSharedPtr<FJsonObject> SpecA = ExportSpecForDiff(SysA);
	TSharedPtr<FJsonObject> SpecB = ExportSpecForDiff(SysB);
	if (!SpecA.IsValid() || !SpecB.IsValid())
		return FMonolithActionResult::Error(TEXT("Failed to export one or both system specs"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system_a"), PathA);
	Result->SetStringField(TEXT("system_b"), PathB);

	// --- System property diffs ---
	TSharedPtr<FJsonObject> SysPropsA = SpecA->GetObjectField(TEXT("system_properties"));
	TSharedPtr<FJsonObject> SysPropsB = SpecB->GetObjectField(TEXT("system_properties"));
	TSharedRef<FJsonObject> SysPropDiff = DiffJsonObjects(SysPropsA, SysPropsB, TEXT("system_properties"));
	if (SysPropDiff->Values.Num() > 0)
		Result->SetObjectField(TEXT("system_property_diffs"), SysPropDiff);

	// --- User parameter diffs ---
	{
		TArray<TSharedPtr<FJsonValue>> ParamDiffs;
		const TArray<TSharedPtr<FJsonValue>>* UPA = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* UPB = nullptr;
		SpecA->TryGetArrayField(TEXT("user_parameters"), UPA);
		SpecB->TryGetArrayField(TEXT("user_parameters"), UPB);

		// Build maps by name
		TMap<FString, TSharedPtr<FJsonObject>> MapA, MapB;
		if (UPA) for (auto& V : *UPA) { auto O = V->AsObject(); if (O) MapA.Add(O->GetStringField(TEXT("name")), O); }
		if (UPB) for (auto& V : *UPB) { auto O = V->AsObject(); if (O) MapB.Add(O->GetStringField(TEXT("name")), O); }

		TSet<FString> AllNames;
		for (auto& P : MapA) AllNames.Add(P.Key);
		for (auto& P : MapB) AllNames.Add(P.Key);

		for (const FString& Name : AllNames)
		{
			TSharedPtr<FJsonObject>* PA = MapA.Find(Name);
			TSharedPtr<FJsonObject>* PB = MapB.Find(Name);
			TSharedRef<FJsonObject> PD = MakeShared<FJsonObject>();
			PD->SetStringField(TEXT("name"), Name);

			if (!PA) { PD->SetStringField(TEXT("change"), TEXT("added_in_b")); ParamDiffs.Add(MakeShared<FJsonValueObject>(PD)); }
			else if (!PB) { PD->SetStringField(TEXT("change"), TEXT("removed_in_b")); ParamDiffs.Add(MakeShared<FJsonValueObject>(PD)); }
			else if (!bSummary)
			{
				// Compare type and default
				bool bDiff = false;
				FString TypeA = (*PA)->GetStringField(TEXT("type"));
				FString TypeB = (*PB)->GetStringField(TEXT("type"));
				if (TypeA != TypeB) { PD->SetStringField(TEXT("type_a"), TypeA); PD->SetStringField(TEXT("type_b"), TypeB); bDiff = true; }
				FString DefA = (*PA)->GetStringField(TEXT("default"));
				FString DefB = (*PB)->GetStringField(TEXT("default"));
				if (DefA != DefB) { PD->SetStringField(TEXT("default_a"), DefA); PD->SetStringField(TEXT("default_b"), DefB); bDiff = true; }
				if (bDiff) { PD->SetStringField(TEXT("change"), TEXT("modified")); ParamDiffs.Add(MakeShared<FJsonValueObject>(PD)); }
			}
		}
		if (ParamDiffs.Num() > 0) Result->SetArrayField(TEXT("parameter_diffs"), ParamDiffs);
	}

	// --- Emitter diffs ---
	{
		TArray<TSharedPtr<FJsonValue>> EmitterDiffs;

		const TArray<TSharedPtr<FJsonValue>>* EA = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* EB = nullptr;
		SpecA->TryGetArrayField(TEXT("emitters"), EA);
		SpecB->TryGetArrayField(TEXT("emitters"), EB);

		TMap<FString, TSharedPtr<FJsonObject>> EmMapA, EmMapB;
		if (EA) for (auto& V : *EA) { auto O = V->AsObject(); if (O) EmMapA.Add(O->GetStringField(TEXT("name")), O); }
		if (EB) for (auto& V : *EB) { auto O = V->AsObject(); if (O) EmMapB.Add(O->GetStringField(TEXT("name")), O); }

		TSet<FString> AllEmitters;
		for (auto& P : EmMapA) AllEmitters.Add(P.Key);
		for (auto& P : EmMapB) AllEmitters.Add(P.Key);

		for (const FString& EmName : AllEmitters)
		{
			TSharedPtr<FJsonObject>* EmA = EmMapA.Find(EmName);
			TSharedPtr<FJsonObject>* EmB = EmMapB.Find(EmName);
			TSharedRef<FJsonObject> EDiff = MakeShared<FJsonObject>();
			EDiff->SetStringField(TEXT("emitter"), EmName);

			if (!EmA) { EDiff->SetStringField(TEXT("change"), TEXT("added_in_b")); EmitterDiffs.Add(MakeShared<FJsonValueObject>(EDiff)); continue; }
			if (!EmB) { EDiff->SetStringField(TEXT("change"), TEXT("removed_in_b")); EmitterDiffs.Add(MakeShared<FJsonValueObject>(EDiff)); continue; }

			bool bEmitterDiff = false;

			// Compare emitter-level properties
			{
				TArray<FString> SimpleProps = { TEXT("sim_target"), TEXT("local_space"), TEXT("determinism"), TEXT("calculate_bounds_mode"), TEXT("allocation_mode") };
				TArray<TSharedPtr<FJsonValue>> PropDiffs;
				for (const FString& Prop : SimpleProps)
				{
					TSharedPtr<FJsonValue> VA = (*EmA)->TryGetField(Prop);
					TSharedPtr<FJsonValue> VB = (*EmB)->TryGetField(Prop);
					FString SA = VA.IsValid() ? (VA->Type == EJson::Boolean ? (VA->AsBool() ? TEXT("true") : TEXT("false")) : VA->AsString()) : TEXT("");
					FString SB = VB.IsValid() ? (VB->Type == EJson::Boolean ? (VB->AsBool() ? TEXT("true") : TEXT("false")) : VB->AsString()) : TEXT("");
					if (SA != SB)
					{
						TSharedRef<FJsonObject> PD = MakeShared<FJsonObject>();
						PD->SetStringField(TEXT("property"), Prop);
						PD->SetStringField(TEXT("a"), SA);
						PD->SetStringField(TEXT("b"), SB);
						PropDiffs.Add(MakeShared<FJsonValueObject>(PD));
					}
				}
				if (PropDiffs.Num() > 0) { EDiff->SetArrayField(TEXT("property_diffs"), PropDiffs); bEmitterDiff = true; }
			}

			if (!bSummary)
			{
				// Compare modules (matched by script path within each stage)
				const TArray<TSharedPtr<FJsonValue>>* ModsA = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* ModsB = nullptr;
				(*EmA)->TryGetArrayField(TEXT("modules"), ModsA);
				(*EmB)->TryGetArrayField(TEXT("modules"), ModsB);

				TArray<TSharedPtr<FJsonValue>> ModDiffs;

				// Build ordered lists per stage
				auto BuildStageMap = [](const TArray<TSharedPtr<FJsonValue>>* Mods) -> TMap<FString, TArray<TSharedPtr<FJsonObject>>>
				{
					TMap<FString, TArray<TSharedPtr<FJsonObject>>> Map;
					if (Mods)
					{
						for (auto& V : *Mods)
						{
							auto O = V->AsObject();
							if (!O) continue;
							FString Stage = O->GetStringField(TEXT("stage"));
							Map.FindOrAdd(Stage).Add(O);
						}
					}
					return Map;
				};

				TMap<FString, TArray<TSharedPtr<FJsonObject>>> StageModsA = BuildStageMap(ModsA);
				TMap<FString, TArray<TSharedPtr<FJsonObject>>> StageModsB = BuildStageMap(ModsB);

				TSet<FString> AllStages;
				for (auto& P : StageModsA) AllStages.Add(P.Key);
				for (auto& P : StageModsB) AllStages.Add(P.Key);

				for (const FString& Stage : AllStages)
				{
					TArray<TSharedPtr<FJsonObject>>* SMA = StageModsA.Find(Stage);
					TArray<TSharedPtr<FJsonObject>>* SMB = StageModsB.Find(Stage);

					// Build script-path indexed maps
					TMap<FString, TSharedPtr<FJsonObject>> ScriptMapA, ScriptMapB;
					if (SMA) for (auto& M : *SMA) ScriptMapA.Add(M->GetStringField(TEXT("script")), M);
					if (SMB) for (auto& M : *SMB) ScriptMapB.Add(M->GetStringField(TEXT("script")), M);

					TSet<FString> AllScripts;
					for (auto& P : ScriptMapA) AllScripts.Add(P.Key);
					for (auto& P : ScriptMapB) AllScripts.Add(P.Key);

					for (const FString& Script : AllScripts)
					{
						TSharedPtr<FJsonObject>* MA = ScriptMapA.Find(Script);
						TSharedPtr<FJsonObject>* MB = ScriptMapB.Find(Script);

						if (!MA)
						{
							TSharedRef<FJsonObject> MD = MakeShared<FJsonObject>();
							MD->SetStringField(TEXT("stage"), Stage);
							MD->SetStringField(TEXT("script"), Script);
							MD->SetStringField(TEXT("change"), TEXT("added_in_b"));
							ModDiffs.Add(MakeShared<FJsonValueObject>(MD));
						}
						else if (!MB)
						{
							TSharedRef<FJsonObject> MD = MakeShared<FJsonObject>();
							MD->SetStringField(TEXT("stage"), Stage);
							MD->SetStringField(TEXT("script"), Script);
							MD->SetStringField(TEXT("change"), TEXT("removed_in_b"));
							ModDiffs.Add(MakeShared<FJsonValueObject>(MD));
						}
						else
						{
							// Compare inputs
							TSharedPtr<FJsonObject> InA = (*MA)->GetObjectField(TEXT("inputs"));
							TSharedPtr<FJsonObject> InB = (*MB)->GetObjectField(TEXT("inputs"));
							TSharedRef<FJsonObject> InDiff = DiffJsonObjects(InA, InB, TEXT("inputs"));
							if (InDiff->Values.Num() > 0)
							{
								TSharedRef<FJsonObject> MD = MakeShared<FJsonObject>();
								MD->SetStringField(TEXT("stage"), Stage);
								MD->SetStringField(TEXT("script"), Script);
								MD->SetStringField(TEXT("change"), TEXT("inputs_differ"));
								MD->SetObjectField(TEXT("input_diffs"), InDiff);
								ModDiffs.Add(MakeShared<FJsonValueObject>(MD));
							}
						}
					}

					// Check ordering
					if (SMA && SMB && SMA->Num() == SMB->Num())
					{
						for (int32 Idx = 0; Idx < SMA->Num(); ++Idx)
						{
							FString ScA = (*SMA)[Idx]->GetStringField(TEXT("script"));
							FString ScB = (*SMB)[Idx]->GetStringField(TEXT("script"));
							if (ScA != ScB)
							{
								TSharedRef<FJsonObject> MD = MakeShared<FJsonObject>();
								MD->SetStringField(TEXT("stage"), Stage);
								MD->SetStringField(TEXT("change"), TEXT("reordered"));
								MD->SetStringField(TEXT("note"), TEXT("Module order differs between systems in this stage"));
								ModDiffs.Add(MakeShared<FJsonValueObject>(MD));
								break;
							}
						}
					}
				}

				if (ModDiffs.Num() > 0) { EDiff->SetArrayField(TEXT("module_diffs"), ModDiffs); bEmitterDiff = true; }

				// Compare renderers
				const TArray<TSharedPtr<FJsonValue>>* RendA = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* RendB = nullptr;
				(*EmA)->TryGetArrayField(TEXT("renderers"), RendA);
				(*EmB)->TryGetArrayField(TEXT("renderers"), RendB);

				int32 RendCountA = RendA ? RendA->Num() : 0;
				int32 RendCountB = RendB ? RendB->Num() : 0;
				if (RendCountA != RendCountB)
				{
					TSharedRef<FJsonObject> RD = MakeShared<FJsonObject>();
					RD->SetNumberField(TEXT("count_a"), RendCountA);
					RD->SetNumberField(TEXT("count_b"), RendCountB);
					EDiff->SetObjectField(TEXT("renderer_diffs"), RD);
					bEmitterDiff = true;
				}
				else if (RendA && RendB)
				{
					TArray<TSharedPtr<FJsonValue>> RendDiffs;
					for (int32 ri = 0; ri < RendCountA; ++ri)
					{
						auto RA = (*RendA)[ri]->AsObject();
						auto RB = (*RendB)[ri]->AsObject();
						if (!RA || !RB) continue;
						FString ClassA = RA->GetStringField(TEXT("class"));
						FString ClassB = RB->GetStringField(TEXT("class"));
						FString MatA = RA->HasField(TEXT("material")) ? RA->GetStringField(TEXT("material")) : TEXT("");
						FString MatB = RB->HasField(TEXT("material")) ? RB->GetStringField(TEXT("material")) : TEXT("");
						if (ClassA != ClassB || MatA != MatB)
						{
							TSharedRef<FJsonObject> RD = MakeShared<FJsonObject>();
							RD->SetNumberField(TEXT("index"), ri);
							if (ClassA != ClassB) { RD->SetStringField(TEXT("class_a"), ClassA); RD->SetStringField(TEXT("class_b"), ClassB); }
							if (MatA != MatB) { RD->SetStringField(TEXT("material_a"), MatA); RD->SetStringField(TEXT("material_b"), MatB); }
							RendDiffs.Add(MakeShared<FJsonValueObject>(RD));
						}
					}
					if (RendDiffs.Num() > 0) { EDiff->SetArrayField(TEXT("renderer_diffs"), RendDiffs); bEmitterDiff = true; }
				}
			}

			if (bEmitterDiff)
			{
				EDiff->SetStringField(TEXT("change"), TEXT("modified"));
				EmitterDiffs.Add(MakeShared<FJsonValueObject>(EDiff));
			}
		}

		if (EmitterDiffs.Num() > 0) Result->SetArrayField(TEXT("emitter_diffs"), EmitterDiffs);
	}

	// Summary stats
	bool bIdentical = !Result->HasField(TEXT("system_property_diffs"))
		&& !Result->HasField(TEXT("parameter_diffs"))
		&& !Result->HasField(TEXT("emitter_diffs"));
	Result->SetBoolField(TEXT("identical"), bIdentical);

	return NA_SuccessObj(Result);
}

// ============================================================================
// Action: save_emitter_as_template (Task 30)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSaveEmitterAsTemplate(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString SavePath = Params->GetStringField(TEXT("save_path"));

	if (EmitterHandleId.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: emitter"));
	if (SavePath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: save_path"));

	// Validate the destination before loading the source system or creating the package.
	if (const FString PathError = MonolithCore::ValidatePackagePath(SavePath); !PathError.IsEmpty())
	{
		return FMonolithActionResult::Error(PathError);
	}

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitter VE = Handle.GetInstance();
	UNiagaraEmitter* SourceEmitter = VE.Emitter;
	if (!SourceEmitter) return FMonolithActionResult::Error(TEXT("Source emitter is null"));

	// Check for existing asset at save path
	const FString AssetName = FPackageName::GetLongPackageAssetName(SavePath);

	// Convert /Game/ path to package name
	FString PackageName = SavePath;

	// Check if asset already exists
	if (FindObject<UPackage>(nullptr, *PackageName))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at: %s"), *PackageName));

	// Create the package
	UPackage* NewPackage = CreatePackage(*PackageName);
	if (!NewPackage) return FMonolithActionResult::Error(TEXT("Failed to create package"));

	// Duplicate the emitter into the new package
	UNiagaraEmitter* NewEmitter = DuplicateObject<UNiagaraEmitter>(SourceEmitter, NewPackage, *AssetName);
	if (!NewEmitter) return FMonolithActionResult::Error(TEXT("DuplicateObject failed"));

	// Set flags
	NewEmitter->SetFlags(RF_Public | RF_Standalone);
	NewEmitter->ClearFlags(RF_Transient);

	// Ensure the graph source outer is reparented to the new package
	FVersionedNiagaraEmitterData* NewED = NewEmitter->GetLatestEmitterData();
	if (NewED)
	{
		if (UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(NewED->GraphSource))
		{
			if (ScriptSource->GetOuter() != NewPackage && ScriptSource->GetOuter() != NewEmitter)
			{
				ScriptSource->Rename(nullptr, NewEmitter);
			}
		}
	}

	// Mark dirty and save
	NewPackage->MarkPackageDirty();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaved = UPackage::SavePackage(NewPackage, NewEmitter, *PackageFilename, SaveArgs);

	if (!bSaved)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Package save failed: %s"), *PackageFilename));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("saved_path"), PackageName);
	R->SetStringField(TEXT("emitter_name"), NewEmitter->GetName());
	R->SetStringField(TEXT("source_system"), SystemPath);
	R->SetStringField(TEXT("source_emitter"), Handle.GetName().ToString());
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: clone_module_overrides (Task 31)
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleCloneModuleOverrides(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString SrcEmitterId = Params->GetStringField(TEXT("source_emitter"));
	FString SrcModuleGuid = Params->GetStringField(TEXT("source_module"));
	FString TgtEmitterId = Params->GetStringField(TEXT("target_emitter"));
	FString TgtModuleGuid = Params->GetStringField(TEXT("target_module"));

	if (SrcModuleGuid.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: source_module"));
	if (TgtModuleGuid.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: target_module"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage SrcUsage, TgtUsage;
	UNiagaraNodeFunctionCall* SrcNode = FindModuleNode(System, SrcEmitterId, SrcModuleGuid, &SrcUsage);
	if (!SrcNode) return FMonolithActionResult::Error(TEXT("Source module node not found"));

	UNiagaraNodeFunctionCall* TgtNode = FindModuleNode(System, TgtEmitterId, TgtModuleGuid, &TgtUsage);
	if (!TgtNode) return FMonolithActionResult::Error(TEXT("Target module node not found"));

	// Validate same script
	FString SrcScript = SrcNode->FunctionScript ? SrcNode->FunctionScript->GetPathName() : TEXT("");
	FString TgtScript = TgtNode->FunctionScript ? TgtNode->FunctionScript->GetPathName() : TEXT("");
	if (SrcScript != TgtScript)
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Source and target modules use different scripts. Source: %s, Target: %s"), *SrcScript, *TgtScript));

	// Get source module inputs
	TArray<FNiagaraVariable> SrcInputs;
	int32 SrcEmitterIdx = FindEmitterHandleIndex(System, SrcEmitterId);
	if (SrcEmitterIdx != INDEX_NONE)
	{
		FVersionedNiagaraEmitter VE = System->GetEmitterHandles()[SrcEmitterIdx].GetInstance();
		FCompileConstantResolver Resolver(VE, SrcUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*SrcNode, SrcInputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}
	else
	{
		FCompileConstantResolver Resolver(System, SrcUsage);
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(*SrcNode, SrcInputs, Resolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly, false);
	}

	TArray<TSharedPtr<FJsonValue>> ClonedArr;
	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	int32 ClonedCount = 0, SkippedCount = 0;

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "CloneOverrides", "Clone Module Overrides"));
	System->Modify();

	for (const FNiagaraVariable& In : SrcInputs)
	{
		FNiagaraParameterHandle SrcAH = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(In.GetName()), SrcNode);
		UEdGraphPin* SrcOP = MonolithNiagaraHelpers::GetStackFunctionInputOverridePin(*SrcNode, SrcAH);
		if (!SrcOP) continue;

		FName ShortName = MonolithNiagaraHelpers::StripModulePrefix(In.GetName());

		if (SrcOP->LinkedTo.Num() > 0)
		{
			// Check what it's linked to
			UEdGraphPin* LinkedPin = SrcOP->LinkedTo[0];
			UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;

			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
			{
				// Parameter binding — use HandleSetModuleInputBinding
				TSharedRef<FJsonObject> BindParams = MakeShared<FJsonObject>();
				BindParams->SetStringField(TEXT("asset_path"), SystemPath);
				BindParams->SetStringField(TEXT("emitter"), TgtEmitterId);
				BindParams->SetStringField(TEXT("module_node"), TgtModuleGuid);
				BindParams->SetStringField(TEXT("input"), ShortName.ToString());
				BindParams->SetStringField(TEXT("binding"), InputNode->Input.GetName().ToString());
				FMonolithActionResult BindResult = HandleSetModuleInputBinding(BindParams);

				TSharedRef<FJsonObject> CO = MakeShared<FJsonObject>();
				CO->SetStringField(TEXT("input"), ShortName.ToString());
				CO->SetStringField(TEXT("type"), TEXT("binding"));
				CO->SetStringField(TEXT("value"), InputNode->Input.GetName().ToString());
				CO->SetBoolField(TEXT("success"), BindResult.bSuccess);
				ClonedArr.Add(MakeShared<FJsonValueObject>(CO));
				if (BindResult.bSuccess) ClonedCount++;
			}
			else if (Cast<UNiagaraNodeFunctionCall>(LinkedNode))
			{
				// Dynamic input — skip
				TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
				SO->SetStringField(TEXT("input"), ShortName.ToString());
				SO->SetStringField(TEXT("reason"), TEXT("dynamic_input"));
				SkippedArr.Add(MakeShared<FJsonValueObject>(SO));
				SkippedCount++;
			}
			else
			{
				// Unknown link type — skip
				TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
				SO->SetStringField(TEXT("input"), ShortName.ToString());
				SO->SetStringField(TEXT("reason"), TEXT("unknown_link_type"));
				SkippedArr.Add(MakeShared<FJsonValueObject>(SO));
				SkippedCount++;
			}
		}
		else if (!SrcOP->DefaultValue.IsEmpty())
		{
			// Inline value — use HandleSetModuleInputValue
			TSharedRef<FJsonObject> ValParams = MakeShared<FJsonObject>();
			ValParams->SetStringField(TEXT("asset_path"), SystemPath);
			ValParams->SetStringField(TEXT("emitter"), TgtEmitterId);
			ValParams->SetStringField(TEXT("module_node"), TgtModuleGuid);
			ValParams->SetStringField(TEXT("input"), ShortName.ToString());
			ValParams->SetStringField(TEXT("value"), SrcOP->DefaultValue);
			FMonolithActionResult ValResult = HandleSetModuleInputValue(ValParams);

			TSharedRef<FJsonObject> CO = MakeShared<FJsonObject>();
			CO->SetStringField(TEXT("input"), ShortName.ToString());
			CO->SetStringField(TEXT("type"), TEXT("inline_value"));
			CO->SetStringField(TEXT("value"), SrcOP->DefaultValue);
			CO->SetBoolField(TEXT("success"), ValResult.bSuccess);
			ClonedArr.Add(MakeShared<FJsonValueObject>(CO));
			if (ValResult.bSuccess) ClonedCount++;
		}
		// Else: no override set — nothing to clone
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("script"), SrcScript);
	R->SetNumberField(TEXT("cloned_count"), ClonedCount);
	R->SetNumberField(TEXT("skipped_count"), SkippedCount);
	R->SetArrayField(TEXT("cloned"), ClonedArr);
	if (SkippedArr.Num() > 0) R->SetArrayField(TEXT("skipped"), SkippedArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 8: Expansion
// ============================================================================

// ============================================================================
// Action: save_system
// Save any Niagara asset to disk. Direct port of material module's SaveMaterial.
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleSaveSystem(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));

	bool bOnlyIfDirty = Params->HasField(TEXT("only_if_dirty")) ? Params->GetBoolField(TEXT("only_if_dirty")) : true;

	// Load via FMonolithAssetUtils which handles path normalization
	UObject* LoadedAsset = FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath);
	if (!LoadedAsset)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));

	// Verify it's a Niagara asset type
	bool bIsNiagara = LoadedAsset->IsA<UNiagaraSystem>()
		|| LoadedAsset->IsA<UNiagaraScript>()
		|| LoadedAsset->IsA<UNiagaraParameterCollection>()
		|| LoadedAsset->IsA<UNiagaraEffectType>();

	if (!bIsNiagara)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is %s — not a Niagara asset type"), *AssetPath, *LoadedAsset->GetClass()->GetName()));

	UPackage* Pkg = LoadedAsset->GetPackage();
	bool bWasDirty = Pkg->IsDirty();

	if (bOnlyIfDirty && !bWasDirty)
	{
		auto ResultJson = MakeShared<FJsonObject>();
		ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
		ResultJson->SetBoolField(TEXT("saved"), false);
		ResultJson->SetBoolField(TEXT("was_dirty"), false);
		ResultJson->SetStringField(TEXT("asset_class"), LoadedAsset->GetClass()->GetName());
		ResultJson->SetStringField(TEXT("message"), TEXT("Asset not dirty — skipped save"));
		return NA_SuccessObj(ResultJson);
	}

	FString PackageFilename = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Pkg, LoadedAsset, *PackageFilename, SaveArgs);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetBoolField(TEXT("saved"), bSaved);
	ResultJson->SetBoolField(TEXT("was_dirty"), bWasDirty);
	ResultJson->SetStringField(TEXT("asset_class"), LoadedAsset->GetClass()->GetName());
	return NA_SuccessObj(ResultJson);
}

// ============================================================================
// Action: get_static_switch_value
// Read mirror of set_static_switch_value — get one or all static switch values.
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleGetStaticSwitchValue(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString ModuleNodeGuid = Params->GetStringField(TEXT("module_node"));
	if (ModuleNodeGuid.IsEmpty()) ModuleNodeGuid = Params->GetStringField(TEXT("module_name"));
	FString InputName = Params->HasField(TEXT("input")) ? Params->GetStringField(TEXT("input")) : FString();

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	if (!EmitterHandleId.IsEmpty() && FindEmitterHandleIndex(System, EmitterHandleId) == INDEX_NONE)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Emitter '%s' not found"), *EmitterHandleId));

	ENiagaraScriptUsage FoundUsage;
	UNiagaraNodeFunctionCall* MN = FindModuleNode(System, EmitterHandleId, ModuleNodeGuid, &FoundUsage);
	if (!MN) return FMonolithActionResult::Error(TEXT("Module node not found"));

	UNiagaraGraph* CalledGraph = MN->GetCalledGraph();
	if (!CalledGraph)
		return FMonolithActionResult::Error(TEXT("Module has no script graph — cannot enumerate static switches"));

	TArray<FNiagaraVariable> SwitchInputs = CalledGraph->FindStaticSwitchInputs();

	if (InputName.IsEmpty())
	{
		// List ALL static switches
		TArray<TSharedPtr<FJsonValue>> SwitchArr;
		for (const FNiagaraVariable& In : SwitchInputs)
		{
			TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
			FString VarName = In.GetName().ToString();
			SO->SetStringField(TEXT("name"), VarName);
			SO->SetStringField(TEXT("type"), In.GetType().GetName());

			// Find the pin to get the current value
			for (UEdGraphPin* Pin : MN->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->GetFName() == In.GetName())
				{
					SO->SetStringField(TEXT("value"), Pin->DefaultValue);
					SO->SetStringField(TEXT("raw_value"), Pin->DefaultValue);
					AddStaticSwitchEnumMetadata(SO, TryGetStaticSwitchEnum(Pin, MN), Pin->DefaultValue);
					break;
				}
			}
			SwitchArr.Add(MakeShared<FJsonValueObject>(SO));
		}

		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("count"), SwitchArr.Num());
		R->SetArrayField(TEXT("switches"), SwitchArr);
		return NA_SuccessObj(R);
	}

	// Find specific switch — same name-matching logic as set_static_switch_value
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
		if (bMatch) { MatchedFullName = In.GetName(); bInputFound = true; break; }
	}

	if (!bInputFound)
	{
		TArray<FString> ValidNames;
		for (const FNiagaraVariable& In : SwitchInputs) ValidNames.Add(In.GetName().ToString());
		if (ValidNames.Num() == 0)
			return FMonolithActionResult::Error(FString::Printf(TEXT("Input '%s' not found — this module has no static switches"), *InputName));
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Static switch '%s' not found. Valid static switches: [%s]"), *InputName, *FString::Join(ValidNames, TEXT(", "))));
	}

	// Find the pin
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
		return FMonolithActionResult::Error(FString::Printf(TEXT("Static switch pin '%s' not found on module node"), *InputName));

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("name"), MatchedFullName.ToString());
	R->SetStringField(TEXT("value"), SwitchPin->DefaultValue);
	R->SetStringField(TEXT("raw_value"), SwitchPin->DefaultValue);
	AddStaticSwitchEnumMetadata(R, TryGetStaticSwitchEnum(SwitchPin, MN), SwitchPin->DefaultValue);
	return NA_SuccessObj(R);
}

// ============================================================================
// Shared helper: ApplySpecToSystem
// Applies user_parameters + emitters + modules + renderers from a JSON spec
// to an existing system. Returns number of failures.
// ============================================================================

int32 FMonolithNiagaraActions::ApplySpecToSystem(UNiagaraSystem* System, const FString& SystemPath,
	const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	int32 FailCount = 0;

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
			if (!AUP.bSuccess) { OutErrors.Add(FString::Printf(TEXT("add_user_parameter: %s"), *AUP.ErrorMessage)); FailCount++; }
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
			if (!AER.bSuccess) { OutErrors.Add(FString::Printf(TEXT("add_emitter: %s"), *AER.ErrorMessage)); FailCount++; continue; }

			FString EmitterId;
			if (AER.Result.IsValid())
				EmitterId = AER.Result->GetStringField(TEXT("handle_id"));
			if (EmitterId.IsEmpty()) continue;

			// Force synchronous compile so the stack graph is fully wired before adding modules
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
					if (!EPR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_emitter_property[%s]: %s"), *P.Key, *EPR.ErrorMessage)); FailCount++; }
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
					if (!AMR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("add_module[%s]: %s"), *MO->GetStringField(TEXT("script")), *AMR.ErrorMessage)); FailCount++; continue; }

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
							if (!SIVR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_module_input[%s]: %s"), *IP.Key, *SIVR.ErrorMessage)); FailCount++; }
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
							if (!SIBR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_module_binding[%s]: %s"), *BP2.Key, *SIBR.ErrorMessage)); FailCount++; }
						}
					}
					// Static switches
					if (MO->HasField(TEXT("static_switches")))
					{
						TSharedPtr<FJsonObject> Switches = MO->GetObjectField(TEXT("static_switches"));
						for (auto& SW : Switches->Values)
						{
							TSharedRef<FJsonObject> SSP = MakeShared<FJsonObject>();
							SSP->SetStringField(TEXT("system_path"), SystemPath);
							SSP->SetStringField(TEXT("emitter"), EmitterId);
							SSP->SetStringField(TEXT("module_node"), NodeGuid);
							SSP->SetStringField(TEXT("input"), SW.Key);
							SSP->SetField(TEXT("value"), SW.Value);
							FMonolithActionResult SSR = HandleSetStaticSwitchValue(SSP);
							if (!SSR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_static_switch[%s]: %s"), *SW.Key, *SSR.ErrorMessage)); FailCount++; }
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
					if (!ARR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("add_renderer[%s]: %s"), *RO->GetStringField(TEXT("class")), *ARR.ErrorMessage)); FailCount++; continue; }

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
						if (!SRMR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_renderer_material: %s"), *SRMR.ErrorMessage)); FailCount++; }
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
							if (!SRPR.bSuccess) { OutErrors.Add(FString::Printf(TEXT("set_renderer_property[%s]: %s"), *RP.Key, *SRPR.ErrorMessage)); FailCount++; }
						}
					}
				}
			}
		}
	}

	return FailCount;
}

// ============================================================================
// Action: import_system_spec
// Overwrite an existing system with a spec (remove all, then apply fresh).
// ============================================================================

FMonolithActionResult FMonolithNiagaraActions::HandleImportSystemSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	if (SystemPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));

	// Parse spec (same logic as create_system_from_spec)
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
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecField->AsString());
		if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
			return FMonolithActionResult::Error(TEXT("Failed to parse spec string"));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("'spec' must be an object"));
	}

	FString Mode = Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")) : TEXT("overwrite");
	if (!Mode.Equals(TEXT("overwrite"), ESearchCase::IgnoreCase) && !Mode.Equals(TEXT("merge"), ESearchCase::IgnoreCase))
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unsupported mode '%s' — use 'overwrite' or 'merge'"), *Mode));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system '%s'"), *SystemPath));

	TArray<FString> Errors;
	int32 FailCount = 0;

	if (Mode.Equals(TEXT("merge"), ESearchCase::IgnoreCase))
	{
		// MERGE MODE: Only add what's new, update what matches
		// User parameters: add only if name doesn't exist
		if (Spec->HasField(TEXT("user_parameters")))
		{
			FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
			TArray<FNiagaraVariable> ExistingParams;
			US.GetUserParameters(ExistingParams);

			TSet<FString> ExistingNames;
			for (const FNiagaraVariable& V : ExistingParams)
			{
				ExistingNames.Add(V.GetName().ToString());
			}

			const TArray<TSharedPtr<FJsonValue>>& ParamArr = Spec->GetArrayField(TEXT("user_parameters"));
			for (const TSharedPtr<FJsonValue>& PVal : ParamArr)
			{
				const TSharedPtr<FJsonObject>* PObj = nullptr;
				if (!PVal->TryGetObject(PObj) || !(*PObj).IsValid()) continue;

				FString ParamName = (*PObj)->GetStringField(TEXT("name"));
				if (!ParamName.StartsWith(TEXT("User."))) ParamName = TEXT("User.") + ParamName;

				if (ExistingNames.Contains(ParamName))
				{
					continue; // Skip existing
				}

				// Add via existing flow
				TSharedRef<FJsonObject> AddParams = MakeShared<FJsonObject>();
				AddParams->SetStringField(TEXT("asset_path"), SystemPath);
				AddParams->SetStringField(TEXT("name"), ParamName);
				AddParams->SetStringField(TEXT("type"), (*PObj)->GetStringField(TEXT("type")));
				if ((*PObj)->HasField(TEXT("default_value")))
					AddParams->SetStringField(TEXT("default_value"), (*PObj)->GetStringField(TEXT("default_value")));
				FMonolithActionResult R = HandleAddUserParameter(AddParams);
				if (!R.bSuccess)
				{
					Errors.Add(FString::Printf(TEXT("Merge: failed to add param '%s': %s"), *ParamName, *R.ErrorMessage));
					FailCount++;
				}
			}
		}

		// Emitters: add new ones, skip existing by name match
		if (Spec->HasField(TEXT("emitters")))
		{
			TSet<FString> ExistingEmitterNames;
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				ExistingEmitterNames.Add(Handle.GetName().ToString());
			}

			// Apply spec — ApplySpecToSystem handles emitter creation and module setup
			// For merge, we only apply emitters whose names don't already exist
			TSharedRef<FJsonObject> FilteredSpec = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> NewEmitters;
			for (const TSharedPtr<FJsonValue>& EVal : Spec->GetArrayField(TEXT("emitters")))
			{
				const TSharedPtr<FJsonObject>* EObj = nullptr;
				if (!EVal->TryGetObject(EObj) || !(*EObj).IsValid()) continue;
				FString EmitterName = (*EObj)->GetStringField(TEXT("name"));
				if (!ExistingEmitterNames.Contains(EmitterName))
				{
					NewEmitters.Add(EVal);
				}
			}

			if (NewEmitters.Num() > 0)
			{
				FilteredSpec->SetArrayField(TEXT("emitters"), NewEmitters);
				FailCount += ApplySpecToSystem(System, SystemPath, FilteredSpec, Errors);
			}
		}
	}
	else
	{
		// OVERWRITE MODE: Remove everything, apply fresh
		GEditor->BeginTransaction(NSLOCTEXT("Monolith", "ImportSpec", "Import System Spec"));
		System->Modify();

		TSet<FGuid> AllHandleIds;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			AllHandleIds.Add(Handle.GetId());
		}
		if (AllHandleIds.Num() > 0)
		{
			System->RemoveEmitterHandlesById(AllHandleIds);
		}

		// Remove all user parameters
		FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
		{
			TArray<FNiagaraVariable> UP;
			US.GetUserParameters(UP);
			for (const FNiagaraVariable& P : UP)
			{
				US.RemoveParameter(P);
			}
		}

		GEditor->EndTransaction();

		// Rebuild graphs after mass removal
		System->RequestCompile(true);
		System->WaitForCompilationComplete();

		FailCount = ApplySpecToSystem(System, SystemPath, Spec, Errors);
	}

	// Final compile
	TSharedRef<FJsonObject> CP = MakeShared<FJsonObject>();
	CP->SetStringField(TEXT("system_path"), SystemPath);
	HandleRequestCompile(CP);

	// Count emitters
	System = LoadSystem(SystemPath); // Reload to get fresh state
	int32 EmitterCount = System ? System->GetEmitterHandles().Num() : 0;

	TSharedRef<FJsonObject> Final = MakeShared<FJsonObject>();
	Final->SetBoolField(TEXT("success"), FailCount == 0);
	Final->SetStringField(TEXT("asset_path"), SystemPath);
	Final->SetStringField(TEXT("mode"), Mode);
	Final->SetNumberField(TEXT("emitter_count"), EmitterCount);
	Final->SetNumberField(TEXT("failed_steps"), FailCount);
	if (Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& E : Errors)
			ErrArr.Add(MakeShared<FJsonValueString>(E));
		Final->SetArrayField(TEXT("errors"), ErrArr);
	}
	return NA_SuccessObj(Final);
}

// ============================================================================
// Phase 9: Medium Priority Expansion
// ============================================================================

// ============================================================================
// Action: get_di_properties — Inspect editable properties on a DI class via CDO
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleGetDIProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString DIClassName = Params->GetStringField(TEXT("di_class"));
	if (DIClassName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: di_class"));

	FString DIDiagnostic;
	UClass* DIC = MonolithNiagaraHelpers::ResolveNiagaraDataInterfaceClass(DIClassName, &DIDiagnostic);
	if (!DIC)
		return FMonolithActionResult::Error(DIDiagnostic.IsEmpty()
			? FString::Printf(TEXT("DI class '%s' not found or not a UNiagaraDataInterface subclass"), *DIClassName)
			: DIDiagnostic);

	UNiagaraDataInterface* CDO = Cast<UNiagaraDataInterface>(DIC->GetDefaultObject());
	if (!CDO) return FMonolithActionResult::Error(TEXT("Failed to get CDO"));

	// Collect editable properties
	TArray<TSharedPtr<FJsonValue>> PropArr;
	for (TFieldIterator<FProperty> It(DIC); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
		PO->SetStringField(TEXT("name"), Prop->GetName());
		PO->SetStringField(TEXT("type"), Prop->GetCPPType());
		PO->SetStringField(TEXT("category"), Prop->GetMetaData(TEXT("Category")));

		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, Prop->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
		if (ValueStr.Len() <= 200)
		{
			PO->SetStringField(TEXT("default_value"), ValueStr);
		}

		PropArr.Add(MakeShared<FJsonValueObject>(PO));
	}

	// Also get DI function signatures
	TArray<FNiagaraFunctionSignature> Sigs;
	CDO->GetFunctionSignatures(Sigs);

	TArray<TSharedPtr<FJsonValue>> FuncArr;
	for (const FNiagaraFunctionSignature& Sig : Sigs)
	{
		TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
		SO->SetStringField(TEXT("name"), Sig.Name.ToString());
		SO->SetBoolField(TEXT("supports_gpu"), Sig.bSupportsGPU);
		SO->SetBoolField(TEXT("supports_cpu"), Sig.bSupportsCPU);

		FText Desc = Sig.GetDescription();
		if (!Desc.IsEmpty()) SO->SetStringField(TEXT("description"), Desc.ToString());

		FuncArr.Add(MakeShared<FJsonValueObject>(SO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("di_class"), DIC->GetName());
	R->SetNumberField(TEXT("property_count"), PropArr.Num());
	R->SetArrayField(TEXT("properties"), PropArr);
	R->SetNumberField(TEXT("function_count"), FuncArr.Num());
	R->SetArrayField(TEXT("functions"), FuncArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: clear_emitter_modules — Remove all modules from an emitter by stage
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleClearEmitterModules(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));
	FString UsageFilter = Params->HasField(TEXT("usage")) ? Params->GetStringField(TEXT("usage")).ToLower() : TEXT("all");

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	// Determine which usages to clear
	TArray<ENiagaraScriptUsage> TargetUsages;
	if (UsageFilter == TEXT("all"))
	{
		TargetUsages.Add(ENiagaraScriptUsage::EmitterSpawnScript);
		TargetUsages.Add(ENiagaraScriptUsage::EmitterUpdateScript);
		TargetUsages.Add(ENiagaraScriptUsage::ParticleSpawnScript);
		TargetUsages.Add(ENiagaraScriptUsage::ParticleUpdateScript);
	}
	else
	{
		ENiagaraScriptUsage SingleUsage;
		if (!ResolveScriptUsage(UsageFilter, SingleUsage))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown usage '%s'. Use: particle_update, particle_spawn, emitter_update, emitter_spawn, or all"), *UsageFilter));
		TargetUsages.Add(SingleUsage);
	}

	FString HandleId = System->GetEmitterHandles()[EIdx].GetId().ToString();

	// Collect all module nodes across target stages
	TMap<FString, int32> PerStageCounts;
	TArray<UNiagaraNodeFunctionCall*> AllModulesToRemove;

	for (ENiagaraScriptUsage Usage : TargetUsages)
	{
		FString StageName = UsageToString(Usage);
		UNiagaraNodeOutput* OutputNode = FindOutputNode(System, HandleId, Usage);
		if (!OutputNode) continue;

		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);

		PerStageCounts.Add(StageName, ModuleNodes.Num());
		AllModulesToRemove.Append(ModuleNodes);
	}

	if (AllModulesToRemove.Num() == 0)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("removed_count"), 0);
		R->SetStringField(TEXT("note"), TEXT("No modules found in specified stage(s)"));
		return NA_SuccessObj(R);
	}

	// Remove in REVERSE order to preserve upstream chain integrity
	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "ClearModules", "Clear Emitter Modules"));
	System->Modify();

	int32 RemovedCount = 0;
	for (int32 i = AllModulesToRemove.Num() - 1; i >= 0; --i)
	{
		FGuid EmitterGuid = System->GetEmitterHandles()[EIdx].GetId();
		if (MonolithNiagaraHelpers::RemoveModuleFromStack(*System, EmitterGuid, *AllModulesToRemove[i]))
		{
			RemovedCount++;
		}
	}

	GEditor->EndTransaction();
	System->RequestCompile(false);
	System->MarkPackageDirty();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("removed_count"), RemovedCount);

	TSharedRef<FJsonObject> Breakdown = MakeShared<FJsonObject>();
	for (const auto& Pair : PerStageCounts)
	{
		Breakdown->SetNumberField(Pair.Key, Pair.Value);
	}
	R->SetObjectField(TEXT("per_stage"), Breakdown);

	return NA_SuccessObj(R);
}

// ============================================================================
// Action: get_module_script_inputs — Pre-add introspection of a module script
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleGetModuleScriptInputs(const TSharedPtr<FJsonObject>& Params)
{
	FString ScriptPath = Params->GetStringField(TEXT("script_path"));
	if (ScriptPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: script_path"));

	UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
	if (!Script)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load script '%s'"), *ScriptPath));

	// Get the latest source and cast to UNiagaraScriptSource to access the NodeGraph
	UNiagaraScriptSourceBase* SourceBase = const_cast<UNiagaraScriptSourceBase*>(Script->GetLatestSource());
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase);
	if (!Source || !Source->NodeGraph)
		return FMonolithActionResult::Error(TEXT("Script has no graph source"));

	UNiagaraGraph* Graph = Source->NodeGraph;

	// Manually iterate graph nodes to find input nodes — UNiagaraGraph::FindInputNodes
	// is NOT exported (no NIAGARAEDITOR_API), so we replicate its logic inline.
	// Only collect Parameter-usage input nodes, filtering duplicates by name.
	TSet<FName> SeenNames;
	TArray<TSharedPtr<FJsonValue>> InputArr;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node);
		if (!InputNode) continue;

		// Only include parameters (not attributes, system constants, or translator constants)
		if (InputNode->Usage != ENiagaraInputNodeUsage::Parameter) continue;

		const FNiagaraVariable& InputVar = InputNode->Input;
		FName FullName = InputVar.GetName();

		// Filter duplicates
		if (SeenNames.Contains(FullName)) continue;
		SeenNames.Add(FullName);

		FString InputName = FullName.ToString();

		// Strip "Module." prefix for consistency with our other APIs
		if (InputName.StartsWith(TEXT("Module.")))
		{
			InputName = InputName.Mid(7);
		}

		TSharedRef<FJsonObject> IO = MakeShared<FJsonObject>();
		IO->SetStringField(TEXT("name"), InputName);
		IO->SetStringField(TEXT("type"), InputVar.GetType().GetName());
		IO->SetStringField(TEXT("usage"), TEXT("parameter"));
		IO->SetBoolField(TEXT("is_parameter"), true);

		InputArr.Add(MakeShared<FJsonValueObject>(IO));
	}

	// Get script metadata from latest version data
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("script_path"), ScriptPath);
	R->SetStringField(TEXT("script_name"), Script->GetName());
	R->SetNumberField(TEXT("input_count"), InputArr.Num());
	R->SetArrayField(TEXT("inputs"), InputArr);

	// Module usage bitmask and metadata from versioned data
	const FVersionedNiagaraScriptData* ScriptData = Script->GetScriptData(Script->GetExposedVersion().VersionGuid);
	if (ScriptData)
	{
		R->SetNumberField(TEXT("module_usage_bitmask"), ScriptData->ModuleUsageBitmask);

		// Decode bitmask to readable stages
		TArray<TSharedPtr<FJsonValue>> ValidStages;
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::ParticleSpawnScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("particle_spawn")));
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::ParticleUpdateScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("particle_update")));
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::EmitterSpawnScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("emitter_spawn")));
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::EmitterUpdateScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("emitter_update")));
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::SystemSpawnScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("system_spawn")));
		if (ScriptData->ModuleUsageBitmask & (1 << (int32)ENiagaraScriptUsage::SystemUpdateScript))
			ValidStages.Add(MakeShared<FJsonValueString>(TEXT("system_update")));
		R->SetArrayField(TEXT("valid_stages"), ValidStages);

		if (!ScriptData->Category.IsEmpty())
			R->SetStringField(TEXT("category"), ScriptData->Category.ToString());

		if (!ScriptData->Description.IsEmpty())
			R->SetStringField(TEXT("description"), ScriptData->Description.ToString());

		R->SetBoolField(TEXT("is_deprecated"), ScriptData->bDeprecated);
		R->SetBoolField(TEXT("is_experimental"), (bool)ScriptData->bExperimental);
		R->SetBoolField(TEXT("is_suggested"), ScriptData->bSuggested);
	}

	return NA_SuccessObj(R);
}

// ============================================================================
// Action: get_scalability_settings — Read scalability from NiagaraEffectType
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleGetScalabilitySettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));

	UNiagaraEffectType* EffectType = LoadObject<UNiagaraEffectType>(nullptr, *AssetPath);
	if (!EffectType)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load effect type '%s'"), *AssetPath));

	const FNiagaraSystemScalabilitySettingsArray& SSArr = EffectType->GetSystemScalabilitySettings();

	TArray<TSharedPtr<FJsonValue>> SettingsArr;
	for (int32 i = 0; i < SSArr.Settings.Num(); i++)
	{
		const FNiagaraSystemScalabilitySettings& S = SSArr.Settings[i];

		TSharedRef<FJsonObject> SO = MakeShared<FJsonObject>();
		SO->SetNumberField(TEXT("index"), i);
		SO->SetNumberField(TEXT("quality_level_mask"), S.Platforms.QualityLevelMask);
		SO->SetBoolField(TEXT("cull_by_distance"), (bool)S.bCullByDistance);
		SO->SetNumberField(TEXT("max_distance"), S.MaxDistance);
		SO->SetBoolField(TEXT("cull_max_instance_count"), (bool)S.bCullMaxInstanceCount);
		SO->SetNumberField(TEXT("max_instances"), S.MaxInstances);
		SO->SetBoolField(TEXT("cull_per_system_max_instance_count"), (bool)S.bCullPerSystemMaxInstanceCount);
		SO->SetNumberField(TEXT("max_system_instances"), S.MaxSystemInstances);
		SO->SetNumberField(TEXT("max_system_proxies"), S.MaxSystemProxies);

		FString CullProxyStr;
		switch (S.CullProxyMode)
		{
		case ENiagaraCullProxyMode::None: CullProxyStr = TEXT("None"); break;
		case ENiagaraCullProxyMode::Instanced_Rendered: CullProxyStr = TEXT("Instanced_Rendered"); break;
		default: CullProxyStr = TEXT("Unknown"); break;
		}
		SO->SetStringField(TEXT("cull_proxy_mode"), CullProxyStr);

		SettingsArr.Add(MakeShared<FJsonValueObject>(SO));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), AssetPath);
	R->SetStringField(TEXT("effect_type"), EffectType->GetName());

	// Top-level effect type properties
	FString UpdateFreqStr;
	switch (EffectType->UpdateFrequency)
	{
	case ENiagaraScalabilityUpdateFrequency::SpawnOnly: UpdateFreqStr = TEXT("SpawnOnly"); break;
	case ENiagaraScalabilityUpdateFrequency::Low: UpdateFreqStr = TEXT("Low"); break;
	case ENiagaraScalabilityUpdateFrequency::Medium: UpdateFreqStr = TEXT("Medium"); break;
	case ENiagaraScalabilityUpdateFrequency::High: UpdateFreqStr = TEXT("High"); break;
	case ENiagaraScalabilityUpdateFrequency::Continuous: UpdateFreqStr = TEXT("Continuous"); break;
	default: UpdateFreqStr = TEXT("Unknown"); break;
	}
	R->SetStringField(TEXT("update_frequency"), UpdateFreqStr);

	FString CullReactionStr;
	switch (EffectType->CullReaction)
	{
	case ENiagaraCullReaction::Deactivate: CullReactionStr = TEXT("Deactivate"); break;
	case ENiagaraCullReaction::DeactivateImmediate: CullReactionStr = TEXT("DeactivateImmediate"); break;
	case ENiagaraCullReaction::DeactivateResume: CullReactionStr = TEXT("DeactivateResume"); break;
	case ENiagaraCullReaction::DeactivateImmediateResume: CullReactionStr = TEXT("DeactivateImmediateResume"); break;
	case ENiagaraCullReaction::PauseResume: CullReactionStr = TEXT("PauseResume"); break;
	default: CullReactionStr = TEXT("Unknown"); break;
	}
	R->SetStringField(TEXT("cull_reaction"), CullReactionStr);
	R->SetBoolField(TEXT("allow_culling_for_local_players"), EffectType->bAllowCullingForLocalPlayers);

	R->SetNumberField(TEXT("setting_count"), SettingsArr.Num());
	R->SetArrayField(TEXT("settings"), SettingsArr);
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: set_scalability_settings — Write scalability on NiagaraEffectType
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleSetScalabilitySettings(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("Missing required param: asset_path"));

	UNiagaraEffectType* EffectType = LoadObject<UNiagaraEffectType>(nullptr, *AssetPath);
	if (!EffectType)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load effect type '%s'"), *AssetPath));

	const TArray<TSharedPtr<FJsonValue>>& SettingsJsonArr = Params->GetArrayField(TEXT("settings"));
	if (SettingsJsonArr.Num() == 0)
		return FMonolithActionResult::Error(TEXT("'settings' array is empty"));

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "SetScalability", "Set Scalability Settings"));
	EffectType->Modify();

	TArray<FNiagaraSystemScalabilitySettings> NewSettings;
	for (const TSharedPtr<FJsonValue>& SettingVal : SettingsJsonArr)
	{
		const TSharedPtr<FJsonObject>* SettingObjPtr = nullptr;
		if (!SettingVal->TryGetObject(SettingObjPtr) || !SettingObjPtr || !(*SettingObjPtr).IsValid())
			continue;

		const TSharedPtr<FJsonObject>& SO = *SettingObjPtr;

		FNiagaraSystemScalabilitySettings S;

		// Build platform set from quality levels
		if (SO->HasField(TEXT("quality_levels")))
		{
			int32 Mask = 0;
			const TArray<TSharedPtr<FJsonValue>>& QLArr = SO->GetArrayField(TEXT("quality_levels"));
			for (const TSharedPtr<FJsonValue>& QL : QLArr)
			{
				int32 Level = (int32)QL->AsNumber();
				if (Level >= 0 && Level <= 4)
					Mask |= (1 << Level);
			}
			S.Platforms = FNiagaraPlatformSet(Mask);
		}
		else if (SO->HasField(TEXT("quality_level_mask")))
		{
			S.Platforms = FNiagaraPlatformSet((int32)SO->GetNumberField(TEXT("quality_level_mask")));
		}
		else
		{
			// Default: all quality levels
			S.Platforms = FNiagaraPlatformSet(INDEX_NONE);
		}

		if (SO->HasField(TEXT("max_distance")))
		{
			S.bCullByDistance = true;
			S.MaxDistance = (float)SO->GetNumberField(TEXT("max_distance"));
		}
		if (SO->HasField(TEXT("max_instances")))
		{
			S.bCullMaxInstanceCount = true;
			S.MaxInstances = (int32)SO->GetNumberField(TEXT("max_instances"));
		}
		if (SO->HasField(TEXT("max_system_instances")))
		{
			S.bCullPerSystemMaxInstanceCount = true;
			S.MaxSystemInstances = (int32)SO->GetNumberField(TEXT("max_system_instances"));
		}
		if (SO->HasField(TEXT("max_system_proxies")))
		{
			S.MaxSystemProxies = (int32)SO->GetNumberField(TEXT("max_system_proxies"));
		}

		NewSettings.Add(S);
	}

	EffectType->SystemScalabilitySettings.Settings = NewSettings;

	GEditor->EndTransaction();
	EffectType->MarkPackageDirty();

#if WITH_EDITOR
	EffectType->PostEditChange();
#endif

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), AssetPath);
	R->SetNumberField(TEXT("setting_count"), NewSettings.Num());
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: list_systems — Search/list NiagaraSystem assets (clone of list_module_scripts)
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleListSystems(const TSharedPtr<FJsonObject>& Params)
{
	FString Search = Params->HasField(TEXT("search")) ? Params->GetStringField(TEXT("search")) : TEXT("");
	FString PathFilter = Params->HasField(TEXT("path")) ? Params->GetStringField(TEXT("path")) : TEXT("");
	int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	if (!PathFilter.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PathFilter));
	}

	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		FString AssetName = Asset.AssetName.ToString();
		FString PackagePath = Asset.GetSoftObjectPath().ToString();

		// Filter by search keyword
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

		// Try to include effect type name if the system is loaded in memory
		UNiagaraSystem* Sys = FindObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (Sys)
		{
			UNiagaraEffectType* ET = Sys->GetEffectType();
			if (ET)
			{
				Entry->SetStringField(TEXT("effect_type"), ET->GetName());
			}
		}

		Results.Add(MakeShared<FJsonValueObject>(Entry));
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetNumberField(TEXT("total_scanned"), Assets.Num());
	R->SetArrayField(TEXT("systems"), Results);
	if (Search.IsEmpty() && Results.Num() >= Limit)
	{
		R->SetStringField(TEXT("note"), TEXT("Results truncated. Use 'search' to narrow down, or increase 'limit'."));
	}
	return NA_SuccessObj(R);
}

// ============================================================================
// Phase 10: Low Priority & QoL
// ============================================================================

// ============================================================================
// Action: duplicate_module
// Composite: add_module (same script) + clone_module_overrides (copy all overrides)
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleDuplicateModule(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString SrcEmitter = Params->GetStringField(TEXT("source_emitter"));
	FString SrcModuleGuid = Params->GetStringField(TEXT("source_module_node"));

	if (SystemPath.IsEmpty() || SrcEmitter.IsEmpty() || SrcModuleGuid.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path, source_emitter, and source_module_node are required"));

	FString TgtEmitter = Params->HasField(TEXT("target_emitter")) ? Params->GetStringField(TEXT("target_emitter")) : SrcEmitter;
	int32 TgtIndex = Params->HasField(TEXT("target_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("target_index"))) : -1;

	// Load system and find source module
	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	ENiagaraScriptUsage SrcUsage;
	FGuid SrcUsageId;
	UNiagaraNodeFunctionCall* SrcNode = FindModuleNode(System, SrcEmitter, SrcModuleGuid, &SrcUsage, &SrcUsageId);
	if (!SrcNode) return FMonolithActionResult::Error(TEXT("Source module node not found"));

	// Get the script path from the source module
	if (!SrcNode->FunctionScript)
		return FMonolithActionResult::Error(TEXT("Source module has no script"));
	FString ScriptPath = SrcNode->FunctionScript->GetPathName();

	// Determine target usage
	FString TgtUsageStr;
	if (Params->HasField(TEXT("target_usage")))
	{
		TgtUsageStr = Params->GetStringField(TEXT("target_usage"));
	}
	else
	{
		TgtUsageStr = UsageToString(SrcUsage);
	}

	// Step 1: Add the module with the same script
	TSharedRef<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("asset_path"), SystemPath);
	AddParams->SetStringField(TEXT("emitter"), TgtEmitter);
	AddParams->SetStringField(TEXT("usage"), TgtUsageStr);
	AddParams->SetStringField(TEXT("module_script"), ScriptPath);
	if (TgtIndex >= 0) AddParams->SetNumberField(TEXT("index"), TgtIndex);
	if (Params->HasField(TEXT("target_stage_name"))) AddParams->SetStringField(TEXT("stage_name"), Params->GetStringField(TEXT("target_stage_name")));
	if (Params->HasField(TEXT("usage_id"))) AddParams->SetStringField(TEXT("usage_id"), Params->GetStringField(TEXT("usage_id")));
	if (Params->HasField(TEXT("stage_index"))) AddParams->SetNumberField(TEXT("stage_index"), Params->GetNumberField(TEXT("stage_index")));
	if (SrcUsage == ENiagaraScriptUsage::ParticleSimulationStageScript && !Params->HasField(TEXT("usage_id"))
		&& !Params->HasField(TEXT("target_stage_name")) && !Params->HasField(TEXT("stage_index")) && SrcUsageId.IsValid())
	{
		AddParams->SetStringField(TEXT("usage_id"), SrcUsageId.ToString());
	}

	FMonolithActionResult AddResult = HandleAddModule(AddParams);
	if (!AddResult.bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to add module: %s"), *AddResult.ErrorMessage));

	FString NewNodeGuid = AddResult.Result->GetStringField(TEXT("node_guid"));

	// Step 2: Clone overrides from source to new module
	TSharedRef<FJsonObject> CloneParams = MakeShared<FJsonObject>();
	CloneParams->SetStringField(TEXT("asset_path"), SystemPath);
	CloneParams->SetStringField(TEXT("source_emitter"), SrcEmitter);
	CloneParams->SetStringField(TEXT("source_module"), SrcModuleGuid);
	CloneParams->SetStringField(TEXT("target_emitter"), TgtEmitter);
	CloneParams->SetStringField(TEXT("target_module"), NewNodeGuid);

	FMonolithActionResult CloneResult = HandleCloneModuleOverrides(CloneParams);

	// Build result
	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("new_node_guid"), NewNodeGuid);
	R->SetStringField(TEXT("script"), ScriptPath);
	R->SetStringField(TEXT("target_emitter"), TgtEmitter);
	R->SetStringField(TEXT("target_usage"), TgtUsageStr);
	if (CloneResult.bSuccess && CloneResult.Result.IsValid())
	{
		R->SetNumberField(TEXT("cloned_overrides"), CloneResult.Result->GetNumberField(TEXT("cloned_count")));
		R->SetNumberField(TEXT("skipped_overrides"), CloneResult.Result->GetNumberField(TEXT("skipped_count")));
	}
	else
	{
		R->SetStringField(TEXT("clone_warning"), FString::Printf(TEXT("Module added but override cloning failed: %s"), *CloneResult.ErrorMessage));
	}
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: get_emitter_parent
// Read-only: returns the parent emitter asset path, or null if no parent
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleGetEmitterParent(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString EmitterHandleId = Params->GetStringField(TEXT("emitter"));

	if (SystemPath.IsEmpty() || EmitterHandleId.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path and emitter are required"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	int32 EIdx = FindEmitterHandleIndex(System, EmitterHandleId);
	if (EIdx == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Emitter not found"));

	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
	FVersionedNiagaraEmitter VersionedEmitter = Handle.GetInstance();
	if (!VersionedEmitter.Emitter) return FMonolithActionResult::Error(TEXT("Emitter instance is null"));

	const FVersionedNiagaraEmitterData* ED = VersionedEmitter.GetEmitterData();
	if (!ED) return FMonolithActionResult::Error(TEXT("Emitter data is null"));

	FVersionedNiagaraEmitter Parent = ED->GetParent();

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("emitter"), Handle.GetName().ToString());

	if (Parent.Emitter)
	{
		R->SetStringField(TEXT("parent_path"), Parent.Emitter->GetPathName());
		R->SetStringField(TEXT("parent_name"), Parent.Emitter->GetName());
		R->SetStringField(TEXT("parent_version"), Parent.Version.ToString());
		R->SetBoolField(TEXT("has_parent"), true);
	}
	else
	{
		R->SetBoolField(TEXT("has_parent"), false);
	}
	return NA_SuccessObj(R);
}

// ============================================================================
// Action: rename_user_parameter
// Renames a user parameter and updates all module input bindings that reference it.
// WARNING: Custom HLSL modules with string references to "User.OldName" will NOT be caught.
// ============================================================================
FMonolithActionResult FMonolithNiagaraActions::HandleRenameUserParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString SystemPath = NA_GetAssetPath(Params);
	FString OldName = Params->GetStringField(TEXT("old_name"));
	FString NewName = Params->GetStringField(TEXT("new_name"));

	if (SystemPath.IsEmpty() || OldName.IsEmpty() || NewName.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path, old_name, and new_name are required"));

	// Strip User. prefix for store lookups
	FString OldSearch = OldName;
	if (OldSearch.StartsWith(TEXT("User."))) OldSearch = OldSearch.Mid(5);
	FString NewSearch = NewName;
	if (NewSearch.StartsWith(TEXT("User."))) NewSearch = NewSearch.Mid(5);

	if (OldSearch.Equals(NewSearch, ESearchCase::IgnoreCase))
		return FMonolithActionResult::Error(TEXT("Old and new names are the same"));

	UNiagaraSystem* System = LoadSystem(SystemPath);
	if (!System) return FMonolithActionResult::Error(TEXT("Failed to load system"));

	// Find the parameter in the user store
	FNiagaraUserRedirectionParameterStore& US = System->GetExposedParameters();
	TArray<FNiagaraVariable> UP;
	US.GetUserParameters(UP);

	FNiagaraVariable FoundVar;
	bool bFound = false;
	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString().Equals(OldSearch, ESearchCase::IgnoreCase))
		{
			FoundVar = P;
			bFound = true;
			break;
		}
	}
	if (!bFound)
		return FMonolithActionResult::Error(FString::Printf(TEXT("User parameter '%s' not found"), *OldName));

	// Check new name doesn't already exist
	for (const FNiagaraVariable& P : UP)
	{
		if (P.GetName().ToString().Equals(NewSearch, ESearchCase::IgnoreCase))
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parameter '%s' already exists"), *NewName));
	}

	FNiagaraTypeDefinition TD = FoundVar.GetType();
	FString OldBindingName = FString::Printf(TEXT("User.%s"), *OldSearch);
	FString NewBindingName = FString::Printf(TEXT("User.%s"), *NewSearch);

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "RenameUP", "Rename User Parameter"));
	System->Modify();

	// Step 1: Add new parameter with same type
	FNiagaraVariable NewVar = MakeUserVariable(NewSearch, TD);
	NewVar.AllocateData();
	// Copy raw data from old param
	if (FoundVar.IsDataAllocated() && FoundVar.GetSizeInBytes() == NewVar.GetSizeInBytes())
	{
		FMemory::Memcpy(NewVar.GetData(), FoundVar.GetData(), FoundVar.GetSizeInBytes());
	}
	US.AddParameter(NewVar, true, true);

	// Step 2: Walk all emitter graphs — find module bindings that reference old parameter
	int32 UpdatedBindings = 0;
	TArray<FString> Warnings;

	for (int32 EIdx = 0; EIdx < System->GetEmitterHandles().Num(); EIdx++)
	{
		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[EIdx];
		FString EmitterName = Handle.GetName().ToString();
		FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
		if (!ED) continue;

		// Check all script stages for this emitter
		TArray<ENiagaraScriptUsage> Usages = {
			ENiagaraScriptUsage::EmitterSpawnScript,
			ENiagaraScriptUsage::EmitterUpdateScript,
			ENiagaraScriptUsage::ParticleSpawnScript,
			ENiagaraScriptUsage::ParticleUpdateScript
		};

		for (ENiagaraScriptUsage Usage : Usages)
		{
			UNiagaraNodeOutput* OutputNode = FindOutputNode(System, EmitterName, Usage);
			if (!OutputNode) continue;

			TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);

			for (UNiagaraNodeFunctionCall* ModNode : ModuleNodes)
			{
				if (!ModNode) continue;

				// Check upstream override node for binding pins linked to NiagaraNodeInput
				UEdGraphPin* PMInput = MonolithNiagaraHelpers::GetParameterMapPin(*ModNode, EGPD_Input);
				if (!PMInput || PMInput->LinkedTo.Num() == 0) continue;

				UEdGraphNode* OverrideNode = PMInput->LinkedTo[0]->GetOwningNode();
				if (!OverrideNode || Cast<UNiagaraNodeFunctionCall>(OverrideNode)) continue;

				for (UEdGraphPin* Pin : OverrideNode->Pins)
				{
					if (Pin->Direction != EGPD_Input) continue;
					if (Pin->LinkedTo.Num() == 0) continue;

					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Pin->LinkedTo[0]->GetOwningNode());
					if (!InputNode) continue;

					FString BoundName = InputNode->Input.GetName().ToString();
					if (BoundName.Equals(OldBindingName, ESearchCase::IgnoreCase))
					{
						// Update the binding to point to new parameter
						InputNode->Modify();
						FNiagaraVariable NewInput = InputNode->Input;
						NewInput.SetName(FName(*NewBindingName));
						InputNode->Input = NewInput;
						UpdatedBindings++;
					}
				}
			}
		}
	}

	// Also check system-level scripts
	{
		TArray<ENiagaraScriptUsage> SysUsages = {
			ENiagaraScriptUsage::SystemSpawnScript,
			ENiagaraScriptUsage::SystemUpdateScript
		};
		for (ENiagaraScriptUsage Usage : SysUsages)
		{
			UNiagaraNodeOutput* OutputNode = FindOutputNode(System, TEXT(""), Usage);
			if (!OutputNode) continue;

			TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
			MonolithNiagaraHelpers::GetOrderedModuleNodes(*OutputNode, ModuleNodes);

			for (UNiagaraNodeFunctionCall* ModNode : ModuleNodes)
			{
				if (!ModNode) continue;
				UEdGraphPin* PMInput = MonolithNiagaraHelpers::GetParameterMapPin(*ModNode, EGPD_Input);
				if (!PMInput || PMInput->LinkedTo.Num() == 0) continue;

				UEdGraphNode* OverrideNode = PMInput->LinkedTo[0]->GetOwningNode();
				if (!OverrideNode || Cast<UNiagaraNodeFunctionCall>(OverrideNode)) continue;

				for (UEdGraphPin* Pin : OverrideNode->Pins)
				{
					if (Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() == 0) continue;
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Pin->LinkedTo[0]->GetOwningNode());
					if (!InputNode) continue;

					FString BoundName = InputNode->Input.GetName().ToString();
					if (BoundName.Equals(OldBindingName, ESearchCase::IgnoreCase))
					{
						InputNode->Modify();
						FNiagaraVariable NewInput = InputNode->Input;
						NewInput.SetName(FName(*NewBindingName));
						InputNode->Input = NewInput;
						UpdatedBindings++;
					}
				}
			}
		}
	}

	// Step 3: Remove old parameter
	US.RemoveParameter(FoundVar);

	GEditor->EndTransaction();
	System->RequestCompile(false);

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetBoolField(TEXT("success"), true);
	R->SetStringField(TEXT("old_name"), OldBindingName);
	R->SetStringField(TEXT("new_name"), NewBindingName);
	R->SetStringField(TEXT("type"), TD.GetName());
	R->SetNumberField(TEXT("updated_bindings"), UpdatedBindings);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		R->SetArrayField(TEXT("warnings"), WarnArr);
	}
	R->SetStringField(TEXT("note"), TEXT("Custom HLSL modules referencing 'User.OldName' in string form are NOT automatically updated"));
	return NA_SuccessObj(R);
}

// ============================================================================
// Tranche 2 (#64): read-only Search & Discovery + per-system DI
//
// All seven actions are pure read-only traversal. Shared building blocks (all verified in-tree
// and re-confirmed against the UE 5.7 offline source index):
//   - ARFilter over UNiagaraSystem::StaticClass()->GetClassPathName()  (clone of HandleListSystems)
//   - UNiagaraSystem::GetExposedParameters() -> FNiagaraUserRedirectionParameterStore&
//        .ReadParameterVariables() -> TArrayView<const FNiagaraVariableWithOffset>   (cf. line ~233)
//   - UNiagaraSystem::GetEmitterHandles() -> const TArray<FNiagaraEmitterHandle>&
//        FNiagaraEmitterHandle::GetEmitterData() -> FVersionedNiagaraEmitterData*
//        ED->SimTarget (ENiagaraSimTarget), ED->GetRenderers(), ED->GraphSource (UNiagaraScriptSource*)
//   - Asset-time renderer material extraction by concrete-type cast: Sprite/Ribbon ->Material,
//        Mesh ->OverrideMaterials[].ExplicitMat   (cf. lines ~8069, ~8620)
//   - FNiagaraDataInterfaceUtilities::ForEachDataInterface(const UNiagaraSystem*, lambda) (header :43)
//   - IAssetRegistry::GetReferencers(FName PackageName, TArray<FName>&, ...) (IAssetRegistry.h:592)
// ============================================================================

namespace
{
	// Build the AR + ARFilter for NiagaraSystem assets, optionally restricted to a content folder.
	IAssetRegistry& GetNiagaraSystemFilter(const FString& FolderFilter, FARFilter& OutFilter)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		OutFilter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
		OutFilter.bRecursiveClasses = true;
		OutFilter.bRecursivePaths = true;
		if (!FolderFilter.IsEmpty())
		{
			OutFilter.PackagePaths.Add(FName(*FolderFilter));
		}
		return AR;
	}

	FString ReadFolderFilter(const TSharedPtr<FJsonObject>& Params)
	{
		return Params->HasField(TEXT("folder")) ? Params->GetStringField(TEXT("folder")) : TEXT("");
	}

	int32 ReadLimit(const TSharedPtr<FJsonObject>& Params, int32 Default)
	{
		return Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : Default;
	}

	// Extract the material referenced by a renderer-properties object at asset time (no runtime instance).
	// Mirrors the proven Sprite/Ribbon/Mesh extraction at lines ~8069 / ~8620.
	UMaterialInterface* GetRendererMaterialAssetTime(UNiagaraRendererProperties* Rend)
	{
		if (!Rend) return nullptr;
		if (UNiagaraSpriteRendererProperties* S = Cast<UNiagaraSpriteRendererProperties>(Rend)) return S->Material;
		if (UNiagaraRibbonRendererProperties* Rib = Cast<UNiagaraRibbonRendererProperties>(Rend)) return Rib->Material;
		if (UNiagaraMeshRendererProperties* MR = Cast<UNiagaraMeshRendererProperties>(Rend))
		{
			if (MR->OverrideMaterials.Num() > 0 && MR->OverrideMaterials[0].ExplicitMat)
			{
				return MR->OverrideMaterials[0].ExplicitMat;
			}
		}
		return nullptr;
	}

	// Collect the set of module-script asset paths used across all emitter graphs of a system.
	// Used as the structural fingerprint for find_similar_systems / query_niagara has_module.
	void CollectSystemModulePaths(UNiagaraSystem* System, TSet<FString>& OutModulePaths)
	{
		if (!System) return;
		TSet<UNiagaraGraph*> Visited;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			if (!ED) continue;
			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(ED->GraphSource);
			if (!Source || !Source->NodeGraph) continue;
			UNiagaraGraph* Graph = Source->NodeGraph;
			if (Visited.Contains(Graph)) continue;
			Visited.Add(Graph);
			for (UEdGraphNode* N : Graph->Nodes)
			{
				UNiagaraNodeFunctionCall* FN = Cast<UNiagaraNodeFunctionCall>(N);
				if (FN && FN->FunctionScript)
				{
					OutModulePaths.Add(FN->FunctionScript->GetPathName());
				}
			}
		}
	}

	// Collect the set of renderer class names across all emitters of a system.
	void CollectSystemRendererClasses(UNiagaraSystem* System, TSet<FString>& OutRendererClasses)
	{
		if (!System) return;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			if (!ED) continue;
			for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
			{
				if (Rend) OutRendererClasses.Add(Rend->GetClass()->GetName());
			}
		}
	}

	// Jaccard similarity of two string sets: |A n B| / |A u B| (empty/empty -> 1.0).
	double JaccardSimilarity(const TSet<FString>& A, const TSet<FString>& B)
	{
		if (A.Num() == 0 && B.Num() == 0) return 1.0;
		int32 Inter = 0;
		for (const FString& E : A) { if (B.Contains(E)) Inter++; }
		const int32 Uni = A.Num() + B.Num() - Inter;
		return Uni > 0 ? static_cast<double>(Inter) / static_cast<double>(Uni) : 0.0;
	}
}

// ----------------------------------------------------------------------------
// search_by_parameter — systems exposing a user parameter matching a name (and optional type)
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleSearchByParameter(const TSharedPtr<FJsonObject>& Params)
{
	const FString ParamQuery = Params->HasField(TEXT("parameter_name")) ? Params->GetStringField(TEXT("parameter_name")) : TEXT("");
	if (ParamQuery.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: parameter_name"));
	const FString TypeQuery = Params->HasField(TEXT("parameter_type")) ? Params->GetStringField(TEXT("parameter_type")) : TEXT("");
	const FString Folder = ReadFolderFilter(Params);
	const int32 Limit = ReadLimit(Params, 50);

	FARFilter Filter;
	IAssetRegistry& AR = GetNiagaraSystemFilter(Folder, Filter);
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		const FString PackagePath = Asset.GetSoftObjectPath().ToString();
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (!Sys) continue;

		FNiagaraUserRedirectionParameterStore& US = Sys->GetExposedParameters();
		TArrayView<const FNiagaraVariableWithOffset> Vars = US.ReadParameterVariables();
		for (const FNiagaraVariableWithOffset& VWO : Vars)
		{
			const FNiagaraVariable& V = VWO; // upcast (matches in-tree idiom at line ~1756)
			const FString VarName = V.GetName().ToString();
			if (!VarName.Contains(ParamQuery, ESearchCase::IgnoreCase)) continue;

			const FString VarType = V.GetType().GetName();
			if (!TypeQuery.IsEmpty() && !VarType.Contains(TypeQuery, ESearchCase::IgnoreCase)) continue;

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), PackagePath);
			Entry->SetStringField(TEXT("param_name"), VarName);
			Entry->SetStringField(TEXT("param_type"), VarType);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			break; // one hit per system is enough
		}
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("parameter_name"), ParamQuery);
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetArrayField(TEXT("systems"), Results);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// search_by_data_interface — systems using a DI whose class name matches the query
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleSearchByDataInterface(const TSharedPtr<FJsonObject>& Params)
{
	const FString DIQuery = Params->HasField(TEXT("di_class")) ? Params->GetStringField(TEXT("di_class")) : TEXT("");
	if (DIQuery.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: di_class"));
	const FString Folder = ReadFolderFilter(Params);
	const int32 Limit = ReadLimit(Params, 50);

	FARFilter Filter;
	IAssetRegistry& AR = GetNiagaraSystemFilter(Folder, Filter);
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		const FString PackagePath = Asset.GetSoftObjectPath().ToString();
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (!Sys) continue;

		bool bMatched = false;
		FString MatchedClass;
		// Asset-time overload (NiagaraDataInterfaceUtilities.h:43). Return false to stop early.
		FNiagaraDataInterfaceUtilities::ForEachDataInterface(Sys,
			[&DIQuery, &bMatched, &MatchedClass](const FNiagaraDataInterfaceUtilities::FDataInterfaceUsageContext& Ctx) -> bool
			{
				if (Ctx.DataInterface)
				{
					const FString ClassName = Ctx.DataInterface->GetClass()->GetName();
					if (ClassName.Contains(DIQuery, ESearchCase::IgnoreCase))
					{
						bMatched = true;
						MatchedClass = ClassName;
						return false; // stop iterating this system
					}
				}
				return true; // continue
			});

		if (bMatched)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), PackagePath);
			Entry->SetStringField(TEXT("di_class"), MatchedClass);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("di_class"), DIQuery);
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetArrayField(TEXT("systems"), Results);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// query_niagara — deterministic structured-filter DSL over all systems
//   Conditions (AND): emitters>N / emitters<N / emitters=N, sim_target=GPU|CPU, has_renderer=<name>
//   Separators: comma and/or the literal "AND" (case-insensitive). NOT natural language.
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleQueryNiagara(const TSharedPtr<FJsonObject>& Params)
{
	const FString QueryString = Params->HasField(TEXT("query_string")) ? Params->GetStringField(TEXT("query_string")) : TEXT("");
	if (QueryString.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: query_string"));
	const FString Folder = ReadFolderFilter(Params);
	const int32 Limit = ReadLimit(Params, 50);

	// Parse conditions. Normalize "AND" to comma, then split on comma.
	FString Normalized = QueryString;
	Normalized.ReplaceInline(TEXT(" AND "), TEXT(","), ESearchCase::IgnoreCase);
	Normalized.ReplaceInline(TEXT(" and "), TEXT(","), ESearchCase::IgnoreCase);
	TArray<FString> RawConds;
	Normalized.ParseIntoArray(RawConds, TEXT(","), true);

	struct FCond { FString Key; FString Op; FString Value; };
	TArray<FCond> Conds;
	for (FString Raw : RawConds)
	{
		Raw.TrimStartAndEndInline();
		if (Raw.IsEmpty()) continue;
		FString Op;
		int32 OpIdx = INDEX_NONE;
		// Check multi-char operators before single-char so ">=" isn't truncated to ">".
		for (const TCHAR* Candidate : { TEXT(">="), TEXT("<="), TEXT(">"), TEXT("<"), TEXT("=") })
		{
			if (Raw.Contains(Candidate, ESearchCase::CaseSensitive))
			{
				OpIdx = Raw.Find(Candidate, ESearchCase::CaseSensitive);
				Op = Candidate;
				break;
			}
		}
		if (OpIdx == INDEX_NONE) continue; // malformed condition skipped
		FCond C;
		C.Key = Raw.Left(OpIdx).TrimStartAndEnd().ToLower();
		C.Op = Op;
		C.Value = Raw.Mid(OpIdx + Op.Len()).TrimStartAndEnd();
		Conds.Add(C);
	}

	if (Conds.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid conditions parsed. Supported: emitters>N / emitters<N / emitters=N, sim_target=GPU|CPU, has_renderer=<name>"));
	}

	FARFilter Filter;
	IAssetRegistry& AR = GetNiagaraSystemFilter(Folder, Filter);
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Matches;
	for (const FAssetData& Asset : Assets)
	{
		const FString PackagePath = Asset.GetSoftObjectPath().ToString();
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (!Sys) continue;

		const int32 EmitterCount = Sys->GetEmitterHandles().Num();
		bool bHasGPU = false, bHasCPU = false;
		TSet<FString> RendererClasses;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			if (!ED) continue;
			if (ED->SimTarget == ENiagaraSimTarget::GPUComputeSim) bHasGPU = true; else bHasCPU = true;
			for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
			{
				if (Rend) RendererClasses.Add(Rend->GetClass()->GetName());
			}
		}

		bool bAllMatch = true;
		for (const FCond& C : Conds)
		{
			bool bThis = false;
			if (C.Key == TEXT("emitters"))
			{
				const int32 N = FCString::Atoi(*C.Value);
				if (C.Op == TEXT("=")) bThis = (EmitterCount == N);
				else if (C.Op == TEXT(">")) bThis = (EmitterCount > N);
				else if (C.Op == TEXT("<")) bThis = (EmitterCount < N);
				else if (C.Op == TEXT(">=")) bThis = (EmitterCount >= N);
				else if (C.Op == TEXT("<=")) bThis = (EmitterCount <= N);
			}
			else if (C.Key == TEXT("sim_target"))
			{
				const FString V = C.Value.ToUpper();
				if (V.StartsWith(TEXT("GPU"))) bThis = bHasGPU;
				else if (V.StartsWith(TEXT("CPU"))) bThis = bHasCPU;
			}
			else if (C.Key == TEXT("has_renderer"))
			{
				for (const FString& RC : RendererClasses)
				{
					if (RC.Contains(C.Value, ESearchCase::IgnoreCase)) { bThis = true; break; }
				}
			}
			if (!bThis) { bAllMatch = false; break; }
		}

		if (bAllMatch)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), PackagePath);
			Entry->SetNumberField(TEXT("emitter_count"), EmitterCount);
			TArray<TSharedPtr<FJsonValue>> SimTargets;
			if (bHasGPU) SimTargets.Add(MakeShared<FJsonValueString>(TEXT("GPU")));
			if (bHasCPU) SimTargets.Add(MakeShared<FJsonValueString>(TEXT("CPU")));
			Entry->SetArrayField(TEXT("sim_targets"), SimTargets);
			Matches.Add(MakeShared<FJsonValueObject>(Entry));
		}
		if (Matches.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("query"), QueryString);
	R->SetNumberField(TEXT("count"), Matches.Num());
	R->SetArrayField(TEXT("matches"), Matches);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// find_similar_systems — rank systems by structural similarity to a reference system.
//   Similarity metric (0..1): weighted blend of three structural signals
//     0.34 * emitter-count proximity (1 - |dA-dB| / max(dA,dB,1))
//     0.33 * Jaccard(renderer-class sets)
//     0.33 * Jaccard(module-script-path sets)
//   The reference scores exactly 1.0 against itself (identical on all three signals).
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleFindSimilarSystems(const TSharedPtr<FJsonObject>& Params)
{
	const FString RefPath = NA_GetAssetPath(Params);
	if (RefPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	const double Threshold = Params->HasField(TEXT("threshold")) ? Params->GetNumberField(TEXT("threshold")) : 0.5;
	const int32 Limit = ReadLimit(Params, 10);

	UNiagaraSystem* RefSys = LoadSystem(RefPath);
	if (!RefSys) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load reference system '%s'"), *RefPath));

	const int32 RefEmitters = RefSys->GetEmitterHandles().Num();
	TSet<FString> RefRenderers; CollectSystemRendererClasses(RefSys, RefRenderers);
	TSet<FString> RefModules; CollectSystemModulePaths(RefSys, RefModules);
	const FString RefPackagePath = RefSys->GetPathName();

	FARFilter Filter;
	IAssetRegistry& AR = GetNiagaraSystemFilter(TEXT(""), Filter);
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	struct FScored { FString Path; double Score; };
	TArray<FScored> Scored;
	for (const FAssetData& Asset : Assets)
	{
		const FString PackagePath = Asset.GetSoftObjectPath().ToString();
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (!Sys) continue;
		const bool bIsSelf = (Sys == RefSys) || (Sys->GetPathName() == RefPackagePath);

		const int32 Emitters = Sys->GetEmitterHandles().Num();
		TSet<FString> Renderers; CollectSystemRendererClasses(Sys, Renderers);
		TSet<FString> Modules; CollectSystemModulePaths(Sys, Modules);

		const int32 MaxE = FMath::Max3(RefEmitters, Emitters, 1);
		const double EmitterProx = 1.0 - (static_cast<double>(FMath::Abs(RefEmitters - Emitters)) / static_cast<double>(MaxE));
		const double RendJac = JaccardSimilarity(RefRenderers, Renderers);
		const double ModJac = JaccardSimilarity(RefModules, Modules);
		const double Score = 0.34 * EmitterProx + 0.33 * RendJac + 0.33 * ModJac;

		if (bIsSelf || Score >= Threshold)
		{
			Scored.Add({ PackagePath, bIsSelf ? 1.0 : Score });
		}
	}

	Scored.Sort([](const FScored& A, const FScored& B) { return A.Score > B.Score; });

	TArray<TSharedPtr<FJsonValue>> Matches;
	for (const FScored& S : Scored)
	{
		if (Matches.Num() >= Limit) break;
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), S.Path);
		Entry->SetNumberField(TEXT("similarity"), S.Score);
		Matches.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), RefPath);
	R->SetNumberField(TEXT("emitter_count"), RefEmitters);
	R->SetNumberField(TEXT("module_count"), RefModules.Num());
	R->SetNumberField(TEXT("count"), Matches.Num());
	R->SetArrayField(TEXT("matches"), Matches);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// search_by_material — systems whose emitter renderers reference a given material
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleSearchByMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath = Params->HasField(TEXT("material_path")) ? Params->GetStringField(TEXT("material_path")) : TEXT("");
	if (MaterialPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: material_path"));
	const FString Folder = ReadFolderFilter(Params);
	const int32 Limit = ReadLimit(Params, 50);

	// Normalize: compare on package-path prefix so /Game/M_Foo and /Game/M_Foo.M_Foo both match.
	FString TargetObjPath = MaterialPath;
	int32 DotIdx;
	if (TargetObjPath.FindChar('.', DotIdx))
	{
		TargetObjPath = TargetObjPath.Left(DotIdx);
	}

	FARFilter Filter;
	IAssetRegistry& AR = GetNiagaraSystemFilter(Folder, Filter);
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : Assets)
	{
		const FString PackagePath = Asset.GetSoftObjectPath().ToString();
		UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *PackagePath);
		if (!Sys) continue;

		bool bMatched = false;
		FString MatchedEmitter;
		for (const FNiagaraEmitterHandle& Handle : Sys->GetEmitterHandles())
		{
			const FVersionedNiagaraEmitterData* ED = Handle.GetEmitterData();
			if (!ED) continue;
			for (UNiagaraRendererProperties* Rend : ED->GetRenderers())
			{
				UMaterialInterface* Mat = GetRendererMaterialAssetTime(Rend);
				if (!Mat) continue;
				FString MatPath = Mat->GetPathName();
				int32 MDot;
				if (MatPath.FindChar('.', MDot)) MatPath = MatPath.Left(MDot);
				if (MatPath == TargetObjPath)
				{
					bMatched = true;
					MatchedEmitter = Handle.GetName().ToString();
					break;
				}
			}
			if (bMatched) break;
		}

		if (bMatched)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), PackagePath);
			Entry->SetStringField(TEXT("emitter"), MatchedEmitter);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
		if (Results.Num() >= Limit) break;
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("material_path"), MaterialPath);
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetArrayField(TEXT("systems"), Results);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// find_niagara_references — assets referencing a given Niagara asset (AR referencer graph)
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleFindNiagaraReferences(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));
	const int32 Limit = ReadLimit(Params, 100);

	// Convert object path (/Game/VFX/NS_Foo or /Game/VFX/NS_Foo.NS_Foo) to long package name (/Game/VFX/NS_Foo).
	FString PackageName = AssetPath;
	int32 DotIdx;
	if (PackageName.FindChar('.', DotIdx))
	{
		PackageName = PackageName.Left(DotIdx);
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FName> Referencers;
	// IAssetRegistry::GetReferencers(FName PackageName, TArray<FName>&, Category=Package) — verified IAssetRegistry.h:592
	AR.GetReferencers(FName(*PackageName), Referencers);

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FName& Ref : Referencers)
	{
		if (Results.Num() >= Limit) break;
		Results.Add(MakeShared<FJsonValueString>(Ref.ToString()));
	}

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), AssetPath);
	R->SetStringField(TEXT("package_name"), PackageName);
	R->SetNumberField(TEXT("count"), Results.Num());
	R->SetNumberField(TEXT("total_referencers"), Referencers.Num());
	R->SetArrayField(TEXT("referencers"), Results);
	return NA_SuccessObj(R);
}

// ----------------------------------------------------------------------------
// list_system_data_interfaces — DIs actually USED BY a given system (per-system traversal).
//   Distinct from get_di_properties (CDO-class reflection only).
// ----------------------------------------------------------------------------
FMonolithActionResult FMonolithNiagaraActions::HandleListSystemDataInterfaces(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = NA_GetAssetPath(Params);
	if (AssetPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required field: asset_path"));

	UNiagaraSystem* Sys = LoadSystem(AssetPath);
	if (!Sys) return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load system '%s'"), *AssetPath));

	TArray<TSharedPtr<FJsonValue>> DIs;
	TSet<FString> Seen; // dedupe by variable+class so repeated DI bindings collapse
	// Asset-time overload (NiagaraDataInterfaceUtilities.h:43). Return true to continue.
	FNiagaraDataInterfaceUtilities::ForEachDataInterface(Sys,
		[&DIs, &Seen](const FNiagaraDataInterfaceUtilities::FDataInterfaceUsageContext& Ctx) -> bool
		{
			if (Ctx.DataInterface)
			{
				const FString ClassName = Ctx.DataInterface->GetClass()->GetName();
				const FString VarName = Ctx.Variable.GetName().ToString();
				const FString Key = VarName + TEXT("|") + ClassName;
				if (!Seen.Contains(Key))
				{
					Seen.Add(Key);
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("variable"), VarName);
					Entry->SetStringField(TEXT("class"), ClassName);
					if (Ctx.OwnerObject)
					{
						Entry->SetStringField(TEXT("owner"), Ctx.OwnerObject->GetName());
					}
					DIs.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			return true; // continue iterating
		});

	TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
	R->SetStringField(TEXT("asset_path"), AssetPath);
	R->SetNumberField(TEXT("count"), DIs.Num());
	R->SetArrayField(TEXT("data_interfaces"), DIs);
	return NA_SuccessObj(R);
}
