#include "MonolithGASInputAuthoringActions.h"

#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputLibrary.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "JsonObjectConverter.h"
#include "MonolithGASInputAssetCommon.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "MonolithGASInputAuthoring"

using namespace MonolithInput;

namespace MonolithInputAuthoring
{
	/** Upper bound on the `properties` object accepted for one instanced subobject. */
	constexpr int32 MaxPropertyKeys = 64;

	/** Upper bound on the class names echoed back when a property name does not resolve. */
	constexpr int32 MaxSuggestedPropertyNames = 32;

	/**
	 * The live mapping array on UInputMappingContext. The legacy `Mappings` member is
	 * deprecated since UE 5.7 and is only migrated in PostLoad for pre-change assets, so a
	 * scripted write to it never reaches GetMappings() — one more way an Enhanced Input edit
	 * can look applied in memory and be absent at runtime.
	 */
	inline FName MappingsPropertyName() { return FName(TEXT("DefaultKeyMappings")); }

	/** Distinguishes the trigger and modifier surfaces without duplicating the flow. */
	struct FKindInfo
	{
		UClass* BaseClass;
		const TCHAR* ClassField;   // add/remove selector, e.g. "modifier_class"
		const TCHAR* ArrayField;   // set_* payload, e.g. "modifiers"
		const TCHAR* ShortPrefix;  // short-name expansion, e.g. "InputModifier"
		const TCHAR* Label;        // human-readable, e.g. "modifier"
		const TCHAR* CountField;   // e.g. "modifier_count"
	};

	inline FKindInfo ModifierKind()
	{
		return { UInputModifier::StaticClass(), TEXT("modifier_class"), TEXT("modifiers"),
			TEXT("InputModifier"), TEXT("modifier"), TEXT("modifier_count") };
	}

	inline FKindInfo TriggerKind()
	{
		return { UInputTrigger::StaticClass(), TEXT("trigger_class"), TEXT("triggers"),
			TEXT("InputTrigger"), TEXT("trigger"), TEXT("trigger_count") };
	}

	/**
	 * Creates an EditInline/Instanced subobject exactly the way the details panel does.
	 *
	 * Outer MUST be the UObject that owns the property. For a per-key modifier that is the
	 * UInputMappingContext — not the UInputAction the mapping points at, and not the
	 * transient package. Only objects whose outer chain lies inside the package being saved
	 * are harvested as exports (FPackageHarvester::TryHarvestExportInternal asserts
	 * FObjectStatus::IsInSavePackage before harvesting); everything else is written as an
	 * import and resolves to null on load. That is the silent modifier loss in issue #140.
	 *
	 * Flags mirror SPropertyEditorEditInline::OnClassPicked: the owner's
	 * RF_PropagateToSubObjects subset, plus RF_ArchetypeObject when the owner is itself a
	 * CDO or archetype. For a normal asset owner that resolves to RF_Public |
	 * RF_Transactional — importantly NOT RF_Transient, which would make the subobject
	 * unsaveable (FSaveContext::IsTransient) and reproduce the same null-on-reload symptom.
	 */
	UObject* NewInstancedSubobject(UObject* Owner, UClass* Class)
	{
		check(Owner && Class);
		EObjectFlags MaskedOuterFlags = Owner->GetMaskedFlags(RF_PropagateToSubObjects);
		if (Owner->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			MaskedOuterFlags |= RF_ArchetypeObject;
		}
		return NewObject<UObject>(Owner, Class, NAME_None, MaskedOuterFlags);
	}

	/** Accepts a /Script path, an exact class name, or a short name ("Negate" -> "InputModifierNegate"). */
	UClass* ResolveInstancedClass(const FString& Spec, const FKindInfo& Kind, FMonolithActionResult& OutError)
	{
		UClass* Found = UClass::TryFindTypeSlow<UClass>(Spec);
		if (!Found && !Spec.Contains(TEXT("/")) && !Spec.Contains(TEXT(".")))
		{
			Found = UClass::TryFindTypeSlow<UClass>(FString(Kind.ShortPrefix) + Spec);
		}
		if (!Found)
		{
			OutError = InvalidParam(Kind.ClassField, FString::Printf(
				TEXT("no class named '%s' (try a full /Script path, or a short name such as 'Negate')"), *Spec));
			return nullptr;
		}
		if (!Found->IsChildOf(Kind.BaseClass))
		{
			OutError = InvalidParam(Kind.ClassField, FString::Printf(
				TEXT("'%s' is not a %s"), *Found->GetPathName(), *Kind.BaseClass->GetName()));
			return nullptr;
		}
		if (Found->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = InvalidParam(Kind.ClassField, FString::Printf(
				TEXT("'%s' is abstract or deprecated and cannot be instanced"), *Found->GetPathName()));
			return nullptr;
		}
		return Found;
	}

	/**
	 * Applies a JSON property bag to a freshly created subobject.
	 *
	 * Unknown property names fail loudly instead of being dropped — a silently ignored
	 * property is the same failure shape as a silently dropped subobject.
	 */
	bool ApplyInstancedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props,
		const TCHAR* Field, FMonolithActionResult& OutError)
	{
		if (!Props.IsValid() || Props->Values.Num() == 0)
		{
			return true;
		}
		if (Props->Values.Num() > MaxPropertyKeys)
		{
			OutError = InvalidParam(Field, FString::Printf(
				TEXT("at most %d properties are accepted, received %d"), MaxPropertyKeys, Props->Values.Num()));
			return false;
		}

		UClass* Class = Target->GetClass();
		for (const auto& Pair : Props->Values)
		{
			const FString Key = MonolithKeyToString(Pair.Key);
			if (Class->FindPropertyByName(FName(*Key)) != nullptr)
			{
				continue;
			}

			TArray<FString> Known;
			for (TFieldIterator<FProperty> It(Class); It && Known.Num() < MaxSuggestedPropertyNames; ++It)
			{
				Known.Add(It->GetName());
			}
			OutError = InvalidParam(Field, FString::Printf(
				TEXT("'%s' has no property '%s'. Known properties (first %d): %s"),
				*Class->GetName(), *Key, Known.Num(), *FString::Join(Known, TEXT(", "))));
			return false;
		}

		FText FailReason;
		if (!FJsonObjectConverter::JsonObjectToUStruct(Props.ToSharedRef(), Class, Target,
			/*CheckFlags=*/0, /*SkipFlags=*/0, /*bStrictMode=*/true, &FailReason))
		{
			OutError = InvalidParam(Field, FString::Printf(
				TEXT("failed to apply properties to %s: %s"), *Class->GetName(), *FailReason.ToString()));
			return false;
		}
		return true;
	}

	/** One `{ class, properties? }` entry of a set_* payload, or a bare class-name string. */
	struct FInstancedSpec
	{
		UClass* Class = nullptr;
		TSharedPtr<FJsonObject> Properties;
	};

	bool ReadInstancedSpecs(const TSharedPtr<FJsonObject>& Params, const FKindInfo& Kind, int32 MaxEntries,
		TArray<FInstancedSpec>& OutSpecs, FMonolithActionResult& OutError)
	{
		OutSpecs.Reset();
		const TSharedPtr<FJsonValue> Field = Params.IsValid() ? Params->TryGetField(Kind.ArrayField) : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Field.IsValid() || Field->Type != EJson::Array || !Field->TryGetArray(Values) || !Values)
		{
			OutError = InvalidParam(Kind.ArrayField,
				TEXT("expected an array of { class, properties? } objects (may be empty to clear)"));
			return false;
		}
		if (Values->Num() > MaxEntries)
		{
			OutError = InvalidParam(Kind.ArrayField, FString::Printf(
				TEXT("at most %d entries are accepted, received %d"), MaxEntries, Values->Num()));
			return false;
		}

		OutSpecs.Reserve(Values->Num());
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			FInstancedSpec Spec;
			FString ClassSpec;
			const TSharedPtr<FJsonObject>* Entry = nullptr;

			// Shorthand: a bare class name means "this class, no property overrides".
			if (Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(ClassSpec))
			{
				if (ClassSpec.IsEmpty())
				{
					OutError = InvalidParam(Kind.ArrayField, FString::Printf(
						TEXT("element %d must be a non-empty class name"), Index));
					return false;
				}
			}
			else if (Value.IsValid() && Value->TryGetObject(Entry) && Entry && Entry->IsValid())
			{
				if (!(*Entry)->TryGetStringField(TEXT("class"), ClassSpec) || ClassSpec.IsEmpty())
				{
					OutError = InvalidParam(Kind.ArrayField, FString::Printf(
						TEXT("element %d is missing a non-empty 'class' field"), Index));
					return false;
				}
				const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
				if ((*Entry)->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
				{
					Spec.Properties = *PropsPtr;
				}
			}
			else
			{
				OutError = InvalidParam(Kind.ArrayField, FString::Printf(
					TEXT("element %d must be a class-name string or a { class, properties? } object"), Index));
				return false;
			}

			Spec.Class = ResolveInstancedClass(ClassSpec, Kind, OutError);
			if (!Spec.Class)
			{
				return false;
			}
			OutSpecs.Add(Spec);
		}
		return true;
	}

	/**
	 * Locates the mapping to edit: either `mapping_index`, or the (input_action, key) pair.
	 *
	 * An ambiguous pair is an explicit error rather than a guess — a context may legitimately
	 * map the same action and key twice with different triggers.
	 */
	bool ResolveMappingIndex(UInputMappingContext* Context, const TSharedPtr<FJsonObject>& Params,
		int32& OutIndex, FMonolithActionResult& OutError)
	{
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		const bool bHasIndex = HasParam(Params, TEXT("mapping_index"));
		const bool bHasAction = HasParam(Params, TEXT("input_action"));
		const bool bHasKey = HasParam(Params, TEXT("key"));

		if (bHasIndex && (bHasAction || bHasKey))
		{
			OutError = InvalidParam(TEXT("mapping_index"),
				TEXT("mapping_index and input_action/key are mutually exclusive"));
			return false;
		}
		if (bHasIndex)
		{
			if (Mappings.IsEmpty())
			{
				OutError = InvalidParam(TEXT("mapping_index"), TEXT("the mapping context has no mappings"));
				return false;
			}
			return ReadBoundedInt(Params, TEXT("mapping_index"), 0, 0, Mappings.Num() - 1, OutIndex, OutError);
		}
		if (!bHasAction || !bHasKey)
		{
			OutError = InvalidParam(TEXT("mapping_index"),
				TEXT("provide mapping_index, or both input_action and key"));
			return false;
		}

		FString ActionPath;
		FString KeyName;
		if (!ReadRequiredString(Params, TEXT("input_action"), ActionPath, OutError)
			|| !ReadRequiredString(Params, TEXT("key"), KeyName, OutError))
		{
			return false;
		}

		UObject* ActionAsset = LoadExact(UInputAction::StaticClass(), ActionPath, TEXT("input_action"),
			TEXT("UInputAction"), OutError);
		if (!ActionAsset)
		{
			return false;
		}
		const UInputAction* Action = CastChecked<UInputAction>(ActionAsset);
		const FKey Key(*KeyName);
		if (!Key.IsValid())
		{
			OutError = InvalidParam(TEXT("key"), FString::Printf(TEXT("'%s' is not a known FKey"), *KeyName));
			return false;
		}

		// Selection has to see every mapping to detect ambiguity, so an oversized context
		// fails explicitly rather than resolving against a partial scan.
		if (Mappings.Num() > MaxMappingScanLimit)
		{
			OutError = InvalidParam(TEXT("input_action"), FString::Printf(
				TEXT("context has %d mappings, above the %d scan bound; select with mapping_index instead"),
				Mappings.Num(), MaxMappingScanLimit));
			return false;
		}

		TArray<int32> Matches;
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (Mappings[Index].Action == Action && Mappings[Index].Key == Key)
			{
				Matches.Add(Index);
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(FString::Printf(
				TEXT("No mapping in '%s' binds '%s' to key '%s'"),
				*Context->GetName(), *Action->GetName(), *KeyName));
			return false;
		}
		if (Matches.Num() > 1)
		{
			TArray<FString> IndexStrings;
			for (int32 Match : Matches)
			{
				IndexStrings.Add(FString::FromInt(Match));
			}
			OutError = InvalidParam(TEXT("input_action"), FString::Printf(
				TEXT("'%s' + '%s' matches %d mappings (indices %s); select with mapping_index"),
				*Action->GetName(), *KeyName, Matches.Num(), *FString::Join(IndexStrings, TEXT(", "))));
			return false;
		}

		OutIndex = Matches[0];
		return true;
	}

	/** Broadcasts the edit, dirties the package, and optionally saves. Fills `saved` on Result. */
	void FinalizeEdit(UObject* Asset, FName ChangedPropertyName, bool bSave,
		const TSharedPtr<FJsonObject>& Result)
	{
		// UInputAction::PostEditChangeProperty dereferences MemberProperty unguarded, so the
		// event is always built from a real FProperty rather than via UObject::PostEditChange().
		if (FProperty* Changed = Asset->GetClass()->FindPropertyByName(ChangedPropertyName))
		{
			FPropertyChangedEvent Event(Changed, EPropertyChangeType::ValueSet);
			Asset->PostEditChangeProperty(Event);
		}
		if (const UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
		{
			UEnhancedInputLibrary::RequestRebuildControlMappingsUsingContext(Context);
		}
		Asset->MarkPackageDirty();

		const bool bSaved = bSave ? UEditorAssetLibrary::SaveLoadedAsset(Asset, false) : false;
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("success"), true);
	}

	/** asset_path + optional mapping selector + save — shared by every mapping-scoped action. */
	FParamSchemaBuilder MappingSchema()
	{
		return FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputMappingContext package or object path"))
			.Optional(TEXT("mapping_index"), TEXT("integer"), TEXT("Index into the context's mappings; mutually exclusive with input_action/key"))
			.OptionalAssetPath(TEXT("input_action"), TEXT("InputAction path identifying the mapping, paired with key"))
			.Optional(TEXT("key"), TEXT("string"), TEXT("FKey name identifying the mapping, paired with input_action (e.g. SpaceBar)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("If true, save the package after the edit. Default: MarkPackageDirty only."), TEXT("false"));
	}

	/** Loads the context and resolves the mapping in one step. */
	UInputMappingContext* OpenMapping(const TSharedPtr<FJsonObject>& Params, int32& OutMappingIndex,
		bool& bOutSave, FMonolithActionResult& OutError)
	{
		FString AssetPath;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, OutError)
			|| !ReadOptionalBool(Params, TEXT("save"), false, bOutSave, OutError))
		{
			return nullptr;
		}
		UObject* Asset = LoadExact(UInputMappingContext::StaticClass(), AssetPath, TEXT("asset_path"),
			TEXT("UInputMappingContext"), OutError);
		if (!Asset)
		{
			return nullptr;
		}
		UInputMappingContext* Context = CastChecked<UInputMappingContext>(Asset);
		return ResolveMappingIndex(Context, Params, OutMappingIndex, OutError) ? Context : nullptr;
	}

	/** Shared result header for a mapping edit. */
	TSharedPtr<FJsonObject> MakeMappingResult(UInputMappingContext* Context, int32 MappingIndex)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Context->GetPathName());
		Result->SetNumberField(TEXT("mapping_index"), MappingIndex);
		Result->SetObjectField(TEXT("mapping"),
			MappingToJson(Context->GetMappings()[MappingIndex], MappingIndex));
		return Result;
	}

	// -------------------------------------------------------------------------
	// Generic array operations, instantiated once per kind.
	// -------------------------------------------------------------------------

	template <typename TObject>
	FMonolithActionResult AddToMapping(const TSharedPtr<FJsonObject>& Params, const FKindInfo& Kind,
		TArray<TObjectPtr<TObject>> FEnhancedActionKeyMapping::* ArrayMember)
	{
		int32 MappingIndex = INDEX_NONE;
		bool bSave = false;
		FMonolithActionResult Error;
		UInputMappingContext* Context = OpenMapping(Params, MappingIndex, bSave, Error);
		if (!Context)
		{
			return Error;
		}

		FString ClassSpec;
		if (!ReadRequiredString(Params, Kind.ClassField, ClassSpec, Error))
		{
			return Error;
		}
		UClass* Class = ResolveInstancedClass(ClassSpec, Kind, Error);
		if (!Class)
		{
			return Error;
		}

		TArray<TObjectPtr<TObject>>& Array = (Context->GetMapping(MappingIndex).*ArrayMember);
		if (Array.Num() >= MaxInstancedPerMapping)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Mapping %d already holds %d %ss, the per-mapping maximum"),
				MappingIndex, Array.Num(), Kind.Label));
		}

		int32 AtIndex = Array.Num();
		if (!ReadBoundedInt(Params, TEXT("at_index"), Array.Num(), 0, Array.Num(), AtIndex, Error))
		{
			return Error;
		}

		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		TSharedPtr<FJsonObject> Props;
		if (Params->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
		{
			Props = *PropsPtr;
		}

		Context->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("AddMappingEntry", "Monolith: Add Input Mapping Entry"));
		Context->Modify();

		// Outer is the mapping context, not the input action — see NewInstancedSubobject.
		TObject* NewEntry = Cast<TObject>(NewInstancedSubobject(Context, Class));
		if (!NewEntry || !ApplyInstancedProperties(NewEntry, Props, TEXT("properties"), Error))
		{
			Transaction.Cancel();
			return NewEntry ? Error : FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to construct %s of class %s"), Kind.Label, *Class->GetPathName()));
		}
		Array.Insert(NewEntry, AtIndex);

		TSharedPtr<FJsonObject> Result = MakeMappingResult(Context, MappingIndex);
		Result->SetStringField(TEXT("kind"), Kind.Label);
		Result->SetStringField(TEXT("class"), Class->GetName());
		Result->SetStringField(TEXT("class_path"), Class->GetPathName());
		Result->SetStringField(TEXT("outer"), NewEntry->GetOuter()->GetPathName());
		Result->SetNumberField(TEXT("index"), AtIndex);
		Result->SetNumberField(Kind.CountField, Array.Num());
		FinalizeEdit(Context, MappingsPropertyName(), bSave, Result);
		return FMonolithActionResult::Success(Result);
	}

	template <typename TObject>
	FMonolithActionResult RemoveFromMapping(const TSharedPtr<FJsonObject>& Params, const FKindInfo& Kind,
		TArray<TObjectPtr<TObject>> FEnhancedActionKeyMapping::* ArrayMember)
	{
		int32 MappingIndex = INDEX_NONE;
		bool bSave = false;
		FMonolithActionResult Error;
		UInputMappingContext* Context = OpenMapping(Params, MappingIndex, bSave, Error);
		if (!Context)
		{
			return Error;
		}

		TArray<TObjectPtr<TObject>>& Array = (Context->GetMapping(MappingIndex).*ArrayMember);
		const bool bHasIndex = HasParam(Params, TEXT("index"));
		const bool bHasClass = HasParam(Params, Kind.ClassField);
		if (bHasIndex == bHasClass)
		{
			return InvalidParam(TEXT("index"), FString::Printf(
				TEXT("provide exactly one of index or %s"), Kind.ClassField));
		}

		int32 RemoveIndex = INDEX_NONE;
		UClass* Class = nullptr;
		if (bHasIndex)
		{
			if (Array.IsEmpty())
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Mapping %d has no %ss to remove"), MappingIndex, Kind.Label));
			}
			if (!ReadBoundedInt(Params, TEXT("index"), 0, 0, Array.Num() - 1, RemoveIndex, Error))
			{
				return Error;
			}
		}
		else
		{
			FString ClassSpec;
			if (!ReadRequiredString(Params, Kind.ClassField, ClassSpec, Error))
			{
				return Error;
			}
			Class = ResolveInstancedClass(ClassSpec, Kind, Error);
			if (!Class)
			{
				return Error;
			}
		}

		Context->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("RemoveMappingEntry", "Monolith: Remove Input Mapping Entry"));
		Context->Modify();

		int32 Removed = 0;
		if (bHasIndex)
		{
			Array.RemoveAt(RemoveIndex);
			Removed = 1;
		}
		else
		{
			Removed = Array.RemoveAll([Class](const TObjectPtr<TObject>& Entry)
			{
				return Entry && Entry->GetClass() == Class;
			});
		}

		if (Removed == 0)
		{
			// Only reachable on the by-class path (the by-index path always removes one).
			// Nothing matched, so report it rather than claiming a successful no-op edit.
			check(Class);
			Transaction.Cancel();
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Mapping %d has no %s of class %s"), MappingIndex, Kind.Label, *Class->GetPathName()));
		}

		TSharedPtr<FJsonObject> Result = MakeMappingResult(Context, MappingIndex);
		Result->SetStringField(TEXT("kind"), Kind.Label);
		Result->SetNumberField(TEXT("removed"), Removed);
		Result->SetNumberField(Kind.CountField, Array.Num());
		FinalizeEdit(Context, MappingsPropertyName(), bSave, Result);
		return FMonolithActionResult::Success(Result);
	}

	template <typename TObject>
	FMonolithActionResult SetMappingArray(const TSharedPtr<FJsonObject>& Params, const FKindInfo& Kind,
		TArray<TObjectPtr<TObject>> FEnhancedActionKeyMapping::* ArrayMember)
	{
		int32 MappingIndex = INDEX_NONE;
		bool bSave = false;
		FMonolithActionResult Error;
		UInputMappingContext* Context = OpenMapping(Params, MappingIndex, bSave, Error);
		if (!Context)
		{
			return Error;
		}

		TArray<FInstancedSpec> Specs;
		if (!ReadInstancedSpecs(Params, Kind, MaxInstancedPerMapping, Specs, Error))
		{
			return Error;
		}

		Context->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("SetMappingArray", "Monolith: Set Input Mapping Entries"));
		Context->Modify();

		TArray<TObjectPtr<TObject>> NewEntries;
		NewEntries.Reserve(Specs.Num());
		for (const FInstancedSpec& Spec : Specs)
		{
			TObject* NewEntry = Cast<TObject>(NewInstancedSubobject(Context, Spec.Class));
			if (!NewEntry || !ApplyInstancedProperties(NewEntry, Spec.Properties, Kind.ArrayField, Error))
			{
				Transaction.Cancel();
				return NewEntry ? Error : FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to construct %s of class %s"), Kind.Label, *Spec.Class->GetPathName()));
			}
			NewEntries.Add(NewEntry);
		}
		(Context->GetMapping(MappingIndex).*ArrayMember) = MoveTemp(NewEntries);

		TSharedPtr<FJsonObject> Result = MakeMappingResult(Context, MappingIndex);
		Result->SetStringField(TEXT("kind"), Kind.Label);
		Result->SetNumberField(Kind.CountField, (Context->GetMapping(MappingIndex).*ArrayMember).Num());
		FinalizeEdit(Context, MappingsPropertyName(), bSave, Result);
		return FMonolithActionResult::Success(Result);
	}

	template <typename TObject>
	FMonolithActionResult SetActionArray(const TSharedPtr<FJsonObject>& Params, const FKindInfo& Kind,
		TArray<TObjectPtr<TObject>> UInputAction::* ArrayMember, FName ChangedPropertyName)
	{
		FString AssetPath;
		bool bSave = false;
		FMonolithActionResult Error;
		if (!ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error)
			|| !ReadOptionalBool(Params, TEXT("save"), false, bSave, Error))
		{
			return Error;
		}
		UObject* Asset = LoadExact(UInputAction::StaticClass(), AssetPath, TEXT("asset_path"),
			TEXT("UInputAction"), Error);
		if (!Asset)
		{
			return Error;
		}
		UInputAction* Action = CastChecked<UInputAction>(Asset);

		TArray<FInstancedSpec> Specs;
		if (!ReadInstancedSpecs(Params, Kind, MaxInstancedPerAction, Specs, Error))
		{
			return Error;
		}

		Action->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(LOCTEXT("SetActionArray", "Monolith: Set Input Action Entries"));
		Action->Modify();

		TArray<TObjectPtr<TObject>> NewEntries;
		NewEntries.Reserve(Specs.Num());
		for (const FInstancedSpec& Spec : Specs)
		{
			TObject* NewEntry = Cast<TObject>(NewInstancedSubobject(Action, Spec.Class));
			if (!NewEntry || !ApplyInstancedProperties(NewEntry, Spec.Properties, Kind.ArrayField, Error))
			{
				Transaction.Cancel();
				return NewEntry ? Error : FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to construct %s of class %s"), Kind.Label, *Spec.Class->GetPathName()));
			}
			NewEntries.Add(NewEntry);
		}
		(Action->*ArrayMember) = MoveTemp(NewEntries);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Action->GetPathName());
		Result->SetStringField(TEXT("kind"), Kind.Label);
		Result->SetNumberField(Kind.CountField, (Action->*ArrayMember).Num());
		Result->SetObjectField(TEXT("action"), InputActionToJson(Action));
		FinalizeEdit(Action, ChangedPropertyName, bSave, Result);
		return FMonolithActionResult::Success(Result);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

void FMonolithGASInputAuthoringActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithInputAuthoring;

	Registry.RegisterAction(TEXT("input"), TEXT("add_mapping_modifier"),
		TEXT("Add an instanced input modifier to one key mapping of an InputMappingContext, outered so it survives save/reload"),
		FMonolithActionHandler::CreateStatic(&HandleAddMappingModifier),
		MappingSchema()
			.Required(TEXT("modifier_class"), TEXT("string"), TEXT("UInputModifier class: /Script path, exact name, or short name (e.g. Negate)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property overrides applied to the new modifier (max 64 keys, unknown names rejected)"))
			.Optional(TEXT("at_index"), TEXT("integer"), TEXT("Insertion position in the modifier array; defaults to append"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("add_mapping_trigger"),
		TEXT("Add an instanced input trigger to one key mapping of an InputMappingContext, outered so it survives save/reload"),
		FMonolithActionHandler::CreateStatic(&HandleAddMappingTrigger),
		MappingSchema()
			.Required(TEXT("trigger_class"), TEXT("string"), TEXT("UInputTrigger class: /Script path, exact name, or short name (e.g. Pulse)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Property overrides applied to the new trigger (max 64 keys, unknown names rejected)"))
			.Optional(TEXT("at_index"), TEXT("integer"), TEXT("Insertion position in the trigger array; defaults to append"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("remove_mapping_modifier"),
		TEXT("Remove a modifier from one key mapping, by array index or by class"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveMappingModifier),
		MappingSchema()
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Modifier array index to remove; mutually exclusive with modifier_class"))
			.Optional(TEXT("modifier_class"), TEXT("string"), TEXT("Remove every modifier of this class; mutually exclusive with index"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("remove_mapping_trigger"),
		TEXT("Remove a trigger from one key mapping, by array index or by class"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveMappingTrigger),
		MappingSchema()
			.Optional(TEXT("index"), TEXT("integer"), TEXT("Trigger array index to remove; mutually exclusive with trigger_class"))
			.Optional(TEXT("trigger_class"), TEXT("string"), TEXT("Remove every trigger of this class; mutually exclusive with index"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_mapping_modifiers"),
		TEXT("Replace the whole modifier array of one key mapping; an empty array clears it"),
		FMonolithActionHandler::CreateStatic(&HandleSetMappingModifiers),
		MappingSchema()
			.Required(TEXT("modifiers"), TEXT("array"), TEXT("Entries as class-name strings or { class, properties? } objects (max 64)"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_mapping_triggers"),
		TEXT("Replace the whole trigger array of one key mapping; an empty array clears it"),
		FMonolithActionHandler::CreateStatic(&HandleSetMappingTriggers),
		MappingSchema()
			.Required(TEXT("triggers"), TEXT("array"), TEXT("Entries as class-name strings or { class, properties? } objects (max 64)"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_input_action_modifiers"),
		TEXT("Replace the modifier array on an InputAction asset; an empty array clears it"),
		FMonolithActionHandler::CreateStatic(&HandleSetInputActionModifiers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputAction package or object path"))
			.Required(TEXT("modifiers"), TEXT("array"), TEXT("Entries as class-name strings or { class, properties? } objects (max 256)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("If true, save the package after the edit. Default: MarkPackageDirty only."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_input_action_triggers"),
		TEXT("Replace the trigger array on an InputAction asset; an empty array clears it"),
		FMonolithActionHandler::CreateStatic(&HandleSetInputActionTriggers),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Canonical InputAction package or object path"))
			.Required(TEXT("triggers"), TEXT("array"), TEXT("Entries as class-name strings or { class, properties? } objects (max 256)"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("If true, save the package after the edit. Default: MarkPackageDirty only."), TEXT("false"))
			.Build());
}

// ─────────────────────────────────────────────────────────────────────────────
// Handlers
// ─────────────────────────────────────────────────────────────────────────────

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleAddMappingModifier(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return AddToMapping<UInputModifier>(Params, ModifierKind(), &FEnhancedActionKeyMapping::Modifiers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleAddMappingTrigger(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return AddToMapping<UInputTrigger>(Params, TriggerKind(), &FEnhancedActionKeyMapping::Triggers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleRemoveMappingModifier(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return RemoveFromMapping<UInputModifier>(Params, ModifierKind(), &FEnhancedActionKeyMapping::Modifiers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleRemoveMappingTrigger(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return RemoveFromMapping<UInputTrigger>(Params, TriggerKind(), &FEnhancedActionKeyMapping::Triggers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleSetMappingModifiers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return SetMappingArray<UInputModifier>(Params, ModifierKind(), &FEnhancedActionKeyMapping::Modifiers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleSetMappingTriggers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return SetMappingArray<UInputTrigger>(Params, TriggerKind(), &FEnhancedActionKeyMapping::Triggers);
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleSetInputActionModifiers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return SetActionArray<UInputModifier>(Params, ModifierKind(), &UInputAction::Modifiers,
		GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers));
}

FMonolithActionResult FMonolithGASInputAuthoringActions::HandleSetInputActionTriggers(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithInputAuthoring;
	return SetActionArray<UInputTrigger>(Params, TriggerKind(), &UInputAction::Triggers,
		GET_MEMBER_NAME_CHECKED(UInputAction, Triggers));
}

#undef LOCTEXT_NAMESPACE
