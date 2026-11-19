// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker implementation. Phase 0 framework primitive.

#include "Reflection/MonolithReflectionWalker.h"
#include "UObject/TextProperty.h"
#include "MonolithJsonUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/Class.h"
#include "Misc/StringOutputDevice.h"
#include "Algo/Count.h"

#if WITH_EDITOR
#include "Kismet2/StructureEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#endif

namespace
{
	// -----------------------------------------------------------------------
	// Robust per-index test for the auto-generated _MAX sentinel.
	//
	// UEnum::NumEnums() counts ALL stored names, INCLUDING the hidden _MAX
	// sentinel — but only when one exists (UEnum::ContainsExistingMax()).
	// Native C++ UENUMs almost always carry a _MAX; UserDefinedEnums usually do
	// NOT (the asset editor manages the enumerator list directly). The old
	// blanket `NumEnums() - 1` therefore dropped the LAST REAL enumerator of any
	// enum without a sentinel — e.g. a 2-value UDS enum yielded only index 0.
	//
	// Engine reference for the conditional form:
	//   PCGAttributeAccessorFactory.cpp:231 —
	//     ContainsExistingMax() ? NumEnums() - 1 : NumEnums()
	//
	// We go one step more defensive: skip a given index ONLY if it is genuinely
	// the sentinel (name ends with "_MAX" AND the enum reports a max). This never
	// drops a real trailing value even if the sentinel were not strictly last,
	// and is a no-op improvement for native enums.
	bool IsAutoMaxSentinel(const UEnum* Enum, int32 Index)
	{
		if (!Enum)
		{
			return false;
		}
		if (!Enum->ContainsExistingMax())
		{
			return false;
		}
		return Enum->GetNameStringByIndex(Index).EndsWith(TEXT("_MAX"), ESearchCase::CaseSensitive);
	}

	// -----------------------------------------------------------------------
	// Compose a representative ImportText token for a single property. Used to
	// build map-entry examples in the POSITIONAL ((K,V),(K,V)) form the engine's
	// FMapProperty::ImportText actually parses (NOT the ((Key=..,Value=..)) form
	// the old hint wrongly advertised). Best effort: enum -> first enumerator,
	// struct -> (Field=..) one level deep, scalars -> a type-appropriate literal,
	// anything else -> "...". Depth guards the struct recursion.
	// -----------------------------------------------------------------------
	FString ComposeExampleToken(const FProperty* Prop, int32 Depth)
	{
		if (!Prop)
		{
			return TEXT("...");
		}

		// Enum: native FEnumProperty, FByteProperty-with-enum, or a UserDefinedEnum
		// recovered from a UDS numeric field. Emit the first non-sentinel enumerator.
		const UEnum* Enum = nullptr;
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			Enum = EnumProp->GetEnum();
		}
		else if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			Enum = ByteProp->Enum;
		}
		if (!Enum)
		{
			Enum = FMonolithReflectionWalker::RecoverUserDefinedEnum(Prop);
		}
		if (Enum)
		{
			const int32 N = Enum->NumEnums();
			for (int32 i = 0; i < N; ++i)
			{
				if (IsAutoMaxSentinel(Enum, i)) continue;
				return Enum->GetNameStringByIndex(i);
			}
			return TEXT("EnumValue");
		}

		// Struct: recurse one level into its fields -> (Field1=..,Field2=..).
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (Depth > 0 && StructProp->Struct)
			{
				TArray<FString> Fields;
				for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
				{
					Fields.Add(FString::Printf(TEXT("%s=%s"), *It->GetName(), *ComposeExampleToken(*It, Depth - 1)));
				}
				if (Fields.Num() > 0)
				{
					return FString::Printf(TEXT("(%s)"), *FString::Join(Fields, TEXT(",")));
				}
			}
			return TEXT("(Field1=...)");
		}

		// Scalars.
		if (Prop->IsA(FBoolProperty::StaticClass()))
		{
			return TEXT("true");
		}
		if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			return NumProp->IsFloatingPoint() ? TEXT("1.0") : TEXT("1");
		}
		if (Prop->IsA(FStrProperty::StaticClass()) || Prop->IsA(FNameProperty::StaticClass()) || Prop->IsA(FTextProperty::StaticClass()))
		{
			return TEXT("\"Text\"");
		}

		return TEXT("...");
	}

	// -----------------------------------------------------------------------
	// Strict numeric text check. FNumericProperty::ImportText_Internal for a
	// floating-point property consumes only [+-.0-9] and then stores
	// Atof(text), so "abc" imports as 0 and reports success; integer import
	// stops at the first non-digit ("12abc" -> 12). This check runs BEFORE the
	// import so a strict caller can refuse the text without touching the value.
	//
	// Accepted float grammar:  ws [+-] (digits [. digits] | . digits) [(e|E) [+-] digits] [f|F] ws
	// Accepted integer grammar: ws [+-] digits ws  |  ws 0x hexdigits ws
	// Integer text that starts with a letter is left to ImportText (enum-name
	// path, which already rejects unknown names).
	// -----------------------------------------------------------------------
	bool MonolithPathWalker_IsCompleteNumericLiteral(const FNumericProperty* NumProp, const FString& InText)
	{
		const FString Text = InText.TrimStartAndEnd();
		const int32 Len = Text.Len();
		if (Len == 0)
		{
			return false;
		}
		int32 i = 0;

		if (NumProp->IsInteger())
		{
			if (FChar::IsAlpha(Text[0]))
			{
				return true; // enum-name import path; ImportText decides.
			}
			if (Len > 2 && Text[0] == TEXT('0') && (Text[1] == TEXT('x') || Text[1] == TEXT('X')))
			{
				for (i = 2; i < Len; ++i)
				{
					if (!FChar::IsHexDigit(Text[i]))
					{
						return false;
					}
				}
				return true;
			}
			if (Text[i] == TEXT('+') || Text[i] == TEXT('-'))
			{
				++i;
			}
			const int32 DigitStart = i;
			while (i < Len && FChar::IsDigit(Text[i]))
			{
				++i;
			}
			return i > DigitStart && i == Len;
		}

		// Floating point.
		if (Text[i] == TEXT('+') || Text[i] == TEXT('-'))
		{
			++i;
		}
		int32 MantissaDigits = 0;
		while (i < Len && FChar::IsDigit(Text[i]))
		{
			++i;
			++MantissaDigits;
		}
		if (i < Len && Text[i] == TEXT('.'))
		{
			++i;
			while (i < Len && FChar::IsDigit(Text[i]))
			{
				++i;
				++MantissaDigits;
			}
		}
		if (MantissaDigits == 0)
		{
			return false;
		}
		if (i < Len && (Text[i] == TEXT('e') || Text[i] == TEXT('E')))
		{
			++i;
			if (i < Len && (Text[i] == TEXT('+') || Text[i] == TEXT('-')))
			{
				++i;
			}
			const int32 ExpStart = i;
			while (i < Len && FChar::IsDigit(Text[i]))
			{
				++i;
			}
			if (i == ExpStart)
			{
				return false;
			}
		}
		if (i < Len && (Text[i] == TEXT('f') || Text[i] == TEXT('F')))
		{
			++i;
		}
		return i == Len;
	}

	// Recursive worker for ListDefaultedStructFields.
	void MonolithPathWalker_CollectDefaultedFields(const UStruct* Struct, const void* ValuePtr, const void* DefaultPtr,
		const FString& Prefix, int32 Depth, TArray<FString>& Out)
	{
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Field = *It;
			const FString Name = Prefix.IsEmpty() ? Field->GetAuthoredName() : Prefix + TEXT(".") + Field->GetAuthoredName();
			if (Field->Identical_InContainer(ValuePtr, DefaultPtr, 0, PPF_None))
			{
				Out.Add(Name);
				continue;
			}
			const FStructProperty* InnerStruct = CastField<FStructProperty>(Field);
			if (InnerStruct && InnerStruct->Struct && Depth > 1)
			{
				MonolithPathWalker_CollectDefaultedFields(InnerStruct->Struct,
					Field->ContainerPtrToValuePtr<void>(ValuePtr), Field->ContainerPtrToValuePtr<void>(DefaultPtr),
					Name, Depth - 1, Out);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Scratch value used by no-mutation path resolves.
// ---------------------------------------------------------------------------
FMonolithReflectionWalker::FPathScratchValue::FPathScratchValue(const FProperty* InProp)
	: Prop(InProp)
{
	check(Prop);
	Memory = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
	Prop->InitializeValue(Memory);
}

FMonolithReflectionWalker::FPathScratchValue::~FPathScratchValue()
{
	if (Prop && Memory)
	{
		Prop->DestroyValue(Memory);
		FMemory::Free(Memory);
	}
}

// ---------------------------------------------------------------------------
// Lookup helper. Mirrors MonolithBlueprintCDOActions.cpp:385-396 — exact match
// first, then case-insensitive iteration. Keeps walker behaviour aligned with
// set_cdo_property so adapters don't surprise callers with name-resolution drift.
// ---------------------------------------------------------------------------
FProperty* FMonolithReflectionWalker::FindPropertyForwarding(UStruct* Struct, const FString& Name)
{
	if (!Struct)
	{
		return nullptr;
	}
	FProperty* P = Struct->FindPropertyByName(FName(*Name));
	if (P)
	{
		return P;
	}
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		if (It->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return *It;
		}
	}
	return nullptr;
}

bool FMonolithReflectionWalker::IsDeprecatedProperty(const FProperty* Prop, FString& OutReason)
{
	OutReason.Reset();
	if (!Prop)
	{
		return false;
	}

	bool bDeprecated = Prop->HasAnyPropertyFlags(CPF_Deprecated);
	FString Message;
#if WITH_METADATA
	if (Prop->HasMetaData(TEXT("DeprecatedProperty")))
	{
		bDeprecated = true;
	}
	if (bDeprecated && Prop->HasMetaData(TEXT("DeprecationMessage")))
	{
		Message = Prop->GetMetaData(TEXT("DeprecationMessage"));
	}
#endif
	if (!bDeprecated)
	{
		return false;
	}

	const UStruct* Owner = Prop->GetOwnerStruct();
	const FString OwnerName = Owner ? Owner->GetName() : FString(TEXT("<unknown>"));
	OutReason = FString::Printf(
		TEXT("field '%s' on %s is deprecated and no longer holds the live data (writes are not read by the engine, reads see an empty or stale value)"),
		*Prop->GetName(), *OwnerName);
	if (!Message.IsEmpty())
	{
		OutReason += FString::Printf(TEXT(": %s"), *Message);
	}

	// UInputMappingContext::Mappings (deprecated in UE 5.7) is the common case: point at
	// the replacement path and the dedicated input actions.
	if (OwnerName == TEXT("InputMappingContext") && Prop->GetName() == TEXT("Mappings"))
	{
		OutReason += TEXT(" Use the path 'DefaultKeyMappings.Mappings[N]' for existing rows, or input_query add_key_mapping / remove_key_mapping to add or remove rows.");
	}
	return true;
}

// ---------------------------------------------------------------------------
// Dotted+bracket path tokeniser.
//
// Splits "Standing.Gaits[Jog].Starts.Forward" into an ordered list of segments:
//   { Member="Standing" }
//   { Member="Gaits", Bracket="Jog", bHasBracket=true }
//   { Member="Starts" }
//   { Member="Forward" }
//
// A bracket binds to the member segment it trails, so "Gaits[Jog]" is ONE
// segment with both a Member ("Gaits") and a bracket key ("Jog"). Multiple
// brackets on one member ("Grid[2][3]") expand to a member then index-only
// segments — supported by emitting bracket-only segments (Member empty,
// bHasBracket true) so chained array-of-array / map-of-array paths resolve.
// ---------------------------------------------------------------------------
namespace
{
	struct FPathSegment
	{
		FString Member;        // Property name to descend into (may be empty for a chained bracket).
		FString Bracket;       // Bracket token ("Jog", "0", ...). Valid only when bHasBracket.
		bool bHasBracket = false;
	};

	// Returns false on malformed syntax (unbalanced brackets, empty member at start).
	bool TokenizePropertyPath(const FString& Path, TArray<FPathSegment>& OutSegments, FString& OutError)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("empty path");
			return false;
		}

		FPathSegment Current;
		bool bHaveCurrent = false;
		auto FlushCurrent = [&]()
		{
			if (bHaveCurrent)
			{
				OutSegments.Add(Current);
				Current = FPathSegment();
				bHaveCurrent = false;
			}
		};

		int32 i = 0;
		const int32 Len = Trimmed.Len();
		while (i < Len)
		{
			const TCHAR C = Trimmed[i];
			if (C == TEXT('.'))
			{
				FlushCurrent();
				++i;
				continue;
			}
			if (C == TEXT('['))
			{
				// A bracket either trails the current member, or (if no member is
				// pending) forms a chained bracket-only segment on the previous result.
				const int32 Close = Trimmed.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
				if (Close == INDEX_NONE)
				{
					OutError = TEXT("unbalanced '[' in path");
					return false;
				}
				const FString Token = Trimmed.Mid(i + 1, Close - i - 1);
				if (bHaveCurrent && !Current.bHasBracket)
				{
					Current.Bracket = Token;
					Current.bHasBracket = true;
				}
				else
				{
					// Chained bracket (e.g. "Grid[2][3]" or "Foo[a][b]"): close out
					// the pending segment and emit a bracket-only segment.
					FlushCurrent();
					FPathSegment BracketOnly;
					BracketOnly.bHasBracket = true;
					BracketOnly.Bracket = Token;
					OutSegments.Add(BracketOnly);
				}
				i = Close + 1;
				continue;
			}
			// Accumulate a member-name character.
			Current.Member.AppendChar(C);
			bHaveCurrent = true;
			++i;
		}
		FlushCurrent();

		if (OutSegments.Num() == 0)
		{
			OutError = TEXT("path resolved to zero segments");
			return false;
		}
		return true;
	}
} // namespace

// ---------------------------------------------------------------------------
// ResolvePath — walk a dotted+bracket path to a single leaf (FProperty*, void*).
//
// At each segment we hold a (CurStruct, CurPtr) cursor. A member descends into
// an FStructProperty's field; a bracket indexes an FArrayProperty (integer) or
// keys an FMapProperty (ImportText through KeyProp). An FObjectProperty member
// is dereferenced to the pointed-at UObject so the next member resolves against
// its class. The terminal leaf's value coercion is the CALLER's job (WriteLeaf),
// so this routine never touches the value grammar — it only navigates.
// ---------------------------------------------------------------------------
FMonolithReflectionWalker::FPathResolveResult FMonolithReflectionWalker::ResolvePath(
	UStruct* TopStruct,
	void* Container,
	const FString& Path,
	bool bCreateMissingKeys)
{
	FPathResolveOptions Options;
	Options.bCreateMissingKeys = bCreateMissingKeys;
	return ResolvePathWithOptions(TopStruct, Container, Path, Options);
}

FMonolithReflectionWalker::FPathResolveResult FMonolithReflectionWalker::ResolvePathWithOptions(
	UStruct* TopStruct,
	void* Container,
	const FString& Path,
	const FPathResolveOptions& Options)
{
	check(IsInGameThread());

	FPathResolveResult Result;
	if (!TopStruct || !Container)
	{
		Result.Error = TEXT("null TopStruct or Container");
		return Result;
	}

	TArray<FPathSegment> Segments;
	if (!TokenizePropertyPath(Path, Segments, Result.Error))
	{
		return Result;
	}

	// Cursor: the struct we currently iterate properties on, and the raw pointer
	// to the live instance of that struct (or the UObject container at the root).
	UStruct* CurStruct = TopStruct;
	void* CurPtr = Container;
	bool bInScratch = false;

	// Object that holds the cursor, and whether its head property is still to be recorded.
	Result.LeafOwnerObject = Options.RootObject;
	bool bOwnerHeadPending = true;

	// Rebuilt path prefix, used to label the created key.
	FString ConsumedPath;

	for (int32 SegIdx = 0; SegIdx < Segments.Num(); ++SegIdx)
	{
		const FPathSegment& Seg = Segments[SegIdx];

		// 1) Resolve the member property on the current struct (if this segment
		//    names one — chained bracket-only segments skip this and operate on the
		//    container property carried from the previous iteration).
		FProperty* MemberProp = nullptr;
		void* MemberPtr = CurPtr;
		if (!Seg.Member.IsEmpty())
		{
			MemberProp = FindPropertyForwarding(CurStruct, Seg.Member);
			if (!MemberProp)
			{
				Result.Error = FString::Printf(TEXT("unknown field '%s' on %s"),
					*Seg.Member, *CurStruct->GetName());
				return Result;
			}
			// A deprecated member resolves, but addresses storage the engine no longer
			// reads — refuse it so a write (or a dry-run size check) cannot silently
			// target the wrong array. Issue #148.
			FString DeprecatedReason;
			if (IsDeprecatedProperty(MemberProp, DeprecatedReason))
			{
				Result.Error = DeprecatedReason;
				return Result;
			}
			MemberPtr = MemberProp->ContainerPtrToValuePtr<void>(CurPtr);
			if (bOwnerHeadPending)
			{
				Result.LeafOwnerHeadProp = MemberProp;
				bOwnerHeadPending = false;
			}
			ConsumedPath += (ConsumedPath.IsEmpty() ? TEXT("") : TEXT(".")) + Seg.Member;
		}
		else
		{
			Result.Error = FString::Printf(TEXT("malformed path: bracket without a preceding member at segment %d"), SegIdx);
			return Result;
		}

		// 2) If this segment has a bracket, the member MUST be an array or map; the
		//    bracket selects the element/value and that becomes the new cursor target.
		FProperty* ResolvedProp = MemberProp;
		void* ResolvedPtr = MemberPtr;

		if (Seg.bHasBracket)
		{
			ConsumedPath += FString::Printf(TEXT("[%s]"), *Seg.Bracket);

			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(MemberProp))
			{
				FScriptArrayHelper Helper(ArrayProp, MemberPtr);
				const FString Token = Seg.Bracket.TrimStartAndEnd();

				if (Token.StartsWith(TEXT("@")))
				{
					// Object selector: [@Name] or [@class=ClassName].
					FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrayProp->Inner);
					if (!InnerObjProp)
					{
						Result.Error = FString::Printf(TEXT("selector '[%s]' is only valid on arrays of object references; '%s' holds %s"),
							*Token, *Seg.Member, *ArrayProp->Inner->GetCPPType());
						return Result;
					}
					FString Selector = Token.Mid(1).TrimStartAndEnd();
					const bool bByClass = Selector.StartsWith(TEXT("class="), ESearchCase::IgnoreCase);
					if (bByClass)
					{
						Selector = Selector.Mid(6).TrimStartAndEnd();
					}
					if (Selector.IsEmpty())
					{
						Result.Error = FString::Printf(TEXT("empty selector '[%s]' on '%s'"), *Token, *Seg.Member);
						return Result;
					}

					TArray<int32> Matches;
					int32 ExactCaseMatch = INDEX_NONE;
					for (int32 ElemIdx = 0; ElemIdx < Helper.Num(); ++ElemIdx)
					{
						const UObject* Elem = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(ElemIdx));
						if (!Elem)
						{
							continue;
						}
						if (bByClass)
						{
							const FString ClassName = Elem->GetClass()->GetName();
							if (ClassName.Equals(Selector, ESearchCase::IgnoreCase)
								|| (TEXT("U") + ClassName).Equals(Selector, ESearchCase::IgnoreCase))
							{
								Matches.Add(ElemIdx);
							}
						}
						else if (Elem->GetName().Equals(Selector, ESearchCase::IgnoreCase))
						{
							Matches.Add(ElemIdx);
							if (ExactCaseMatch == INDEX_NONE && Elem->GetName().Equals(Selector, ESearchCase::CaseSensitive))
							{
								ExactCaseMatch = ElemIdx;
							}
						}
					}

					// Name list for error messages (first 10 elements).
					auto DescribeElements = [&Helper, InnerObjProp]()
					{
						TArray<FString> Names;
						for (int32 ElemIdx = 0; ElemIdx < Helper.Num() && Names.Num() < 10; ++ElemIdx)
						{
							const UObject* Elem = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(ElemIdx));
							Names.Add(Elem ? FString::Printf(TEXT("[%d] %s (%s)"), ElemIdx, *Elem->GetName(), *Elem->GetClass()->GetName())
								: FString::Printf(TEXT("[%d] None"), ElemIdx));
						}
						if (Helper.Num() > Names.Num())
						{
							Names.Add(FString::Printf(TEXT("... %d more"), Helper.Num() - Names.Num()));
						}
						return FString::Join(Names, TEXT(", "));
					};

					int32 Chosen = INDEX_NONE;
					if (Matches.Num() == 1)
					{
						Chosen = Matches[0];
					}
					else if (Matches.Num() > 1 && !bByClass && ExactCaseMatch != INDEX_NONE)
					{
						Chosen = ExactCaseMatch;
					}
					else if (Matches.Num() > 1)
					{
						Result.Error = FString::Printf(TEXT("selector '[%s]' on '%s' matched %d elements; use '[@ObjectName]' or an index. Elements: %s"),
							*Token, *Seg.Member, Matches.Num(), *DescribeElements());
						return Result;
					}
					else
					{
						Result.Error = FString::Printf(TEXT("selector '[%s]' matched no element of '%s' (size %d). Elements: %s"),
							*Token, *Seg.Member, Helper.Num(), *DescribeElements());
						return Result;
					}
					ResolvedProp = ArrayProp->Inner;
					ResolvedPtr = Helper.GetRawPtr(Chosen);
				}
				else
				{
					if (!Token.IsNumeric())
					{
						Result.Error = FString::Printf(TEXT("array '%s' requires an integer index, got '[%s]' (arrays of object references also accept '[@ObjectName]' or '[@class=ClassName]')"),
							*Seg.Member, *Seg.Bracket);
						return Result;
					}
					const int32 Index = FCString::Atoi(*Token);
					if (Index < 0 || Index >= Helper.Num())
					{
						Result.Error = FString::Printf(TEXT("array index %d out of range on '%s' (size %d)"),
							Index, *Seg.Member, Helper.Num());
						return Result;
					}
					ResolvedProp = ArrayProp->Inner;
					ResolvedPtr = Helper.GetRawPtr(Index);
				}
			}
			else if (FMapProperty* MapProp = CastField<FMapProperty>(MemberProp))
			{
				FScriptMapHelper Helper(MapProp, MemberPtr);

				// Build a scratch key from the bracket token via the key's ImportText
				// grammar — this makes enum names, ints, FName/string keys all work.
				void* KeyTemp = FMemory::Malloc(MapProp->KeyProp->GetSize(), MapProp->KeyProp->GetMinAlignment());
				MapProp->KeyProp->InitializeValue(KeyTemp);
				FStringOutputDevice KeyErr;
				const TCHAR* KeyOk = MapProp->KeyProp->ImportText_Direct(*Seg.Bracket, KeyTemp, nullptr, PPF_None, &KeyErr);
				if (!KeyOk)
				{
					MapProp->KeyProp->DestroyValue(KeyTemp);
					FMemory::Free(KeyTemp);
					Result.Error = FString::Printf(TEXT("map key '[%s]' on '%s' rejected by key grammar: %s"),
						*Seg.Bracket, *Seg.Member, *KeyErr);
					return Result;
				}

				int32 InternalIndex = Helper.FindMapIndexWithKey(KeyTemp);
				bool bCreatedHere = false;
				void* CreatedValuePtr = nullptr;
				if (InternalIndex == INDEX_NONE)
				{
					if (!Options.bCreateMissingKeys)
					{
						MapProp->KeyProp->DestroyValue(KeyTemp);
						FMemory::Free(KeyTemp);
						Result.Error = FString::Printf(TEXT("map key '[%s]' not found on '%s' (pass create_missing_keys=true to add it)"),
							*Seg.Bracket, *Seg.Member);
						return Result;
					}

					// Default-initialised value for the new entry, optionally seeded from
					// NewElementValue (first created entry only).
					TSharedPtr<FPathScratchValue> NewValue = MakeShared<FPathScratchValue>(MapProp->ValueProp);
					if (!Result.bCreatedMissingKey && Options.NewElementValue.IsValid())
					{
						FBulkFillSpec SeedSpec;
						SeedSpec.bStrict = Options.bStrict;
						FDryRunReport SeedReport;
						const FBulkFillFieldWrite Seed = WriteLeaf(MapProp->ValueProp, NewValue->Memory,
							Options.NewElementValue, Options.RootObject, SeedSpec, SeedReport, ConsumedPath);
						if (!Seed.bOk)
						{
							MapProp->KeyProp->DestroyValue(KeyTemp);
							FMemory::Free(KeyTemp);
							Result.Error = FString::Printf(TEXT("new_element_value rejected for map value type %s at '%s': %s"),
								*MapProp->ValueProp->GetCPPType(), *ConsumedPath, *Seed.Reason);
							return Result;
						}
						for (const FBulkFillFieldWrite& Drop : SeedReport.SilentDrops)
						{
							Result.Warnings.Add(FString::Printf(TEXT("new_element_value: %s"), *Drop.Reason));
						}
						Result.bAppliedNewElementValue = true;
					}

					if (Options.bNoMutation || bInScratch)
					{
						// Dry run (or already inside a scratch value): walk on in the scratch value.
						CreatedValuePtr = NewValue->Memory;
						Result.ScratchValues.Add(NewValue);
						bInScratch = true;
					}
					else
					{
						Helper.AddPair(KeyTemp, NewValue->Memory);  // clones key + value
						Helper.Rehash();
						InternalIndex = Helper.FindMapIndexWithKey(KeyTemp);
						if (InternalIndex != INDEX_NONE)
						{
							CreatedValuePtr = Helper.GetValuePtr(InternalIndex);
						}
					}
					bCreatedHere = true;
				}
				else if (!Result.bCreatedMissingKey && Options.NewElementValue.IsValid() && Options.bCreateMissingKeys)
				{
					Result.Warnings.Add(FString::Printf(TEXT("new_element_value ignored: key '[%s]' already exists on '%s'"),
						*Seg.Bracket, *Seg.Member));
				}

				MapProp->KeyProp->DestroyValue(KeyTemp);
				FMemory::Free(KeyTemp);

				if (bCreatedHere)
				{
					if (!CreatedValuePtr)
					{
						Result.Error = FString::Printf(TEXT("internal error: map key '[%s]' on '%s' could not be located after insert"),
							*Seg.Bracket, *Seg.Member);
						return Result;
					}
					if (!Result.bCreatedMissingKey)
					{
						Result.bCreatedMissingKey = true;
						Result.CreatedKeyPath = ConsumedPath;
						Result.CreatedElementProp = MapProp->ValueProp;
						Result.CreatedElementPtr = CreatedValuePtr;
						Result.bLeafIsCreatedElement = (SegIdx == Segments.Num() - 1);
					}
					ResolvedPtr = CreatedValuePtr;
				}
				else
				{
					ResolvedPtr = Helper.GetValuePtr(InternalIndex);
				}
				ResolvedProp = MapProp->ValueProp;
			}
			else
			{
				Result.Error = FString::Printf(TEXT("field '%s' is not an array or map, cannot apply '[%s]'"),
					*Seg.Member, *Seg.Bracket);
				return Result;
			}
		}

		// 3) Last segment -> this is the leaf. Return it for WriteLeaf.
		if (SegIdx == Segments.Num() - 1)
		{
			Result.bOk = true;
			Result.LeafProp = ResolvedProp;
			Result.LeafPtr = ResolvedPtr;
			Result.LeafTypeName = ResolvedProp->GetCPPType();
			Result.bLeafInScratch = bInScratch;
			return Result;
		}

		// 4) Not the leaf -> the resolved property must be descendable: a struct
		//    (continue into its members) or an object ref (deref to its class).
		if (FStructProperty* StructProp = CastField<FStructProperty>(ResolvedProp))
		{
			CurStruct = StructProp->Struct;
			CurPtr = ResolvedPtr;
			continue;
		}
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(ResolvedProp))
		{
			UObject* Pointee = ObjProp->GetObjectPropertyValue(ResolvedPtr);
			if (!Pointee)
			{
				Result.Error = FString::Printf(TEXT("cannot descend through '%s': object reference is null"),
					*ConsumedPath);
				return Result;
			}
			if (Options.HomePackage && Pointee->GetPackage() != Options.HomePackage)
			{
				Result.ExternalObjects.Add(Pointee->GetPathName());
				if (Options.bRefuseExternalObjects)
				{
					Result.Error = FString::Printf(
						TEXT("'%s' refers to '%s', which lives in package '%s', outside the target asset's package '%s'. "
							 "Writes do not follow references into other packages; pass allow_external=true to write there anyway."),
						*ConsumedPath, *Pointee->GetPathName(), *Pointee->GetPackage()->GetName(), *Options.HomePackage->GetName());
					return Result;
				}
			}
			CurStruct = Pointee->GetClass();
			CurPtr = Pointee;
			// Leaving scratch memory: an object reached from a scratch value is a live object.
			bInScratch = false;
			Result.LeafOwnerObject = Pointee;
			Result.LeafOwnerHeadProp = nullptr;
			bOwnerHeadPending = true;
			continue;
		}

		Result.Error = FString::Printf(TEXT("cannot descend through '%s' (type %s) — only structs and object refs are traversable"),
			*Seg.Member, *ResolvedProp->GetCPPType());
		return Result;
	}

	// Unreachable: the leaf return inside the loop always fires on the last segment.
	Result.Error = TEXT("internal error: path produced no leaf");
	return Result;
}

// ---------------------------------------------------------------------------
// InspectLeaf — dry-run leaf evaluation against a copy of the live value.
// ---------------------------------------------------------------------------
FBulkFillFieldWrite FMonolithReflectionWalker::InspectLeaf(
	const FPathResolveResult& Resolved,
	const TSharedPtr<FJsonValue>& JsonVal,
	UObject* Owner,
	const FBulkFillSpec& Spec,
	FDryRunReport& OutReport,
	const FString& PathLabel)
{
	if (!Resolved.bOk || !Resolved.LeafProp || !Resolved.LeafPtr)
	{
		FBulkFillFieldWrite Write;
		Write.Path = PathLabel;
		Write.bOk = false;
		Write.Reason = TEXT("path not resolved");
		return Write;
	}

	if (Resolved.bLeafInScratch)
	{
		// The leaf is inside a scratch value the resolve created; writing it is side-effect free.
		return WriteLeaf(Resolved.LeafProp, Resolved.LeafPtr, JsonVal, Owner, Spec, OutReport, PathLabel);
	}

	FPathScratchValue Scratch(Resolved.LeafProp);
	Resolved.LeafProp->CopyCompleteValue(Scratch.Memory, Resolved.LeafPtr);
	return WriteLeaf(Resolved.LeafProp, Scratch.Memory, JsonVal, Owner, Spec, OutReport, PathLabel);
}

// ---------------------------------------------------------------------------
// ListDefaultedStructFields — which fields of a struct value still equal the
// struct default. Used to report what a leaf-only map-entry create left unset.
// ---------------------------------------------------------------------------
TArray<FString> FMonolithReflectionWalker::ListDefaultedStructFields(const FProperty* ElementProp, const void* ElementPtr, int32 MaxDepth)
{
	TArray<FString> Out;
	const FStructProperty* StructProp = CastField<FStructProperty>(ElementProp);
	if (!StructProp || !StructProp->Struct || !ElementPtr || MaxDepth <= 0)
	{
		return Out;
	}
	FPathScratchValue Defaults(StructProp);
	MonolithPathWalker_CollectDefaultedFields(StructProp->Struct, ElementPtr, Defaults.Memory, FString(), MaxDepth, Out);
	return Out;
}

// ---------------------------------------------------------------------------
// WriteLeaf — public forwarder onto DispatchByPropertyType so the surgical-path
// handler reuses the exact bulk-walker value coercion (one source of truth for
// scalars / enums / structs / arrays / maps / sets / hard + soft refs).
// ---------------------------------------------------------------------------
FBulkFillFieldWrite FMonolithReflectionWalker::WriteLeaf(
	FProperty* LeafProp,
	void* LeafPtr,
	const TSharedPtr<FJsonValue>& JsonVal,
	UObject* Owner,
	const FBulkFillSpec& Spec,
	FDryRunReport& OutReport,
	const FString& PathLabel)
{
	FBulkFillFieldWrite Write;
	Write.Path = PathLabel;
	if (!LeafProp || !LeafPtr)
	{
		Write.bOk = false;
		Write.Reason = TEXT("null leaf property or pointer");
		return Write;
	}
	DispatchByPropertyType(LeafProp, LeafPtr, JsonVal, Owner, Spec, OutReport, PathLabel, Write);
	return Write;
}

// ---------------------------------------------------------------------------
// Recover the UEnum backing a UserDefinedEnum field inside a UserDefinedStruct.
//
// A UDS field of UserDefinedEnum type compiles to a plain numeric FProperty with
// NO Enum association (UUserDefinedEnum is always ECppForm::Namespaced; the
// KismetCompiler emits FEnumProperty only for ECppForm::EnumClass). So
// CastField<FEnumProperty> returns null and GetCPPType() yields "int32". The
// UEnum survives only in editor-only UDS metadata:
//   GetGuidFromPropertyName(VarName) -> GetVarDescByGuid -> SubCategoryObject.
//
// Native enums (FEnumProperty) and FByteProperty-with-enum are deliberately NOT
// matched here — they already carry their UEnum and need no recovery.
// ---------------------------------------------------------------------------
UEnum* FMonolithReflectionWalker::RecoverUserDefinedEnum(const FProperty* Prop)
{
#if WITH_EDITOR
	if (!Prop)
	{
		return nullptr;
	}

	// Native enums already surface as FEnumProperty (or FByteProperty with Enum) —
	// they are not the broken case and must keep their existing handling.
	if (Prop->IsA(FEnumProperty::StaticClass()))
	{
		return nullptr;
	}
	if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			return nullptr;
		}
	}

	// Only the numeric integer/byte fall-through can hide a UDS enum.
	if (!Prop->IsA(FNumericProperty::StaticClass()))
	{
		return nullptr;
	}

	const UUserDefinedStruct* UDStruct = Cast<const UUserDefinedStruct>(Prop->GetOwnerStruct());
	if (!UDStruct)
	{
		return nullptr;
	}

	const FGuid VarGuid = FStructureEditorUtils::GetGuidFromPropertyName(Prop->GetFName());
	if (!VarGuid.IsValid())
	{
		return nullptr;
	}

	const FStructVariableDescription* VarDesc =
		FStructureEditorUtils::GetVarDescByGuid(UDStruct, VarGuid);
	if (!VarDesc)
	{
		return nullptr;
	}

	// SubCategoryObject is a TSoftObjectPtr<UObject>; the enum asset is loaded at
	// editor time. LoadSynchronous resolves it (no-op if already in memory).
	return Cast<UEnum>(VarDesc->SubCategoryObject.LoadSynchronous());
#else
	return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Map an incoming token (display name / authored name / bare integer) to the
// integer VALUE of a recovered UDS UserDefinedEnum field. One implementation,
// shared by every write site so resolution behaviour cannot drift.
//
// Bare-integer tokens return false on purpose: callers then fall through to their
// existing ImportText path, preserving back-compat for numeric authoring.
// ---------------------------------------------------------------------------
bool FMonolithReflectionWalker::ResolveUserDefinedEnumToken(const FProperty* Prop, const FString& Token, int64& OutValue)
{
#if WITH_EDITOR
	UEnum* Enum = RecoverUserDefinedEnum(Prop);
	if (!Enum)
	{
		return false;
	}

	const FString Trimmed = Token.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return false;
	}

	// Bare integer -> defer to the existing numeric ImportText path (back-compat).
	if (Trimmed.IsNumeric())
	{
		return false;
	}

	// 1) Authored short/full name (GetValueByNameString handles both).
	const int64 ByName = Enum->GetValueByNameString(Trimmed);
	if (ByName != INDEX_NONE)
	{
		OutValue = ByName;
		return true;
	}

	// 2) Friendly display-name scan (UserDefinedEnum display names differ from the
	//    authored short names, which carry a GUID-ish suffix). Iterate EVERY index
	//    and skip only the genuine auto-_MAX sentinel, so non-first values
	//    (e.g. "Heavy") resolve — the old `NumEnums() - 1` dropped the last value
	//    of any enum without a sentinel.
	const int32 NumEntries = Enum->NumEnums();
	for (int32 i = 0; i < NumEntries; ++i)
	{
		if (IsAutoMaxSentinel(Enum, i))
		{
			continue;
		}
		if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(Trimmed, ESearchCase::IgnoreCase))
		{
			OutValue = Enum->GetValueByIndex(i);
			return true;
		}
	}

	return false;
#else
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Inner switch — routes a single JSON value to its FProperty subtype handler.
// Order matters: most-derived subclasses tested first (FEnumProperty before the
// generic numeric path), object refs distinguished from soft refs by CastField.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::DispatchByPropertyType(
	FProperty* Prop,
	void* ValuePtr,
	const TSharedPtr<FJsonValue>& JsonVal,
	UObject* Owner,
	const FBulkFillSpec& Spec,
	FDryRunReport& OutReport,
	const FString& PathPrefix,
	FBulkFillFieldWrite& OutWrite)
{
	if (!Prop || !JsonVal.IsValid())
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("null property or json value");
		return;
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		WriteEnum(EnumProp, ValuePtr, JsonVal, OutWrite);
		return;
	}
	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		WriteArray(ArrayProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		WriteMap(MapProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		WriteSet(SetProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		WriteStruct(StructProp, ValuePtr, JsonVal, Owner, Spec, OutReport, PathPrefix, OutWrite);
		return;
	}
	if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
	{
		WriteSoftObjectRef(SoftProp, ValuePtr, JsonVal, OutWrite);
		return;
	}
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		WriteObjectRef(ObjProp, ValuePtr, JsonVal, Owner, OutWrite);
		return;
	}
	// Fall-through: scalar (int, float, bool, FName, FString, FText, byte, enum-as-byte).
	WriteScalar(Prop, ValuePtr, JsonVal, Owner, OutWrite, &Spec, &OutReport);
}

// ---------------------------------------------------------------------------
// Scalar write — stringify JSON then ImportText_Direct (matches existing
// MonolithBlueprintCDOActions.cpp:451-475 path so behaviour matches set_cdo_property).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteScalar(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, FBulkFillFieldWrite& OutWrite, const FBulkFillSpec* Spec, FDryRunReport* OutReport)
{
	FString ValStr;
	if (JsonVal->Type == EJson::Number)
	{
		ValStr = FString::SanitizeFloat(JsonVal->AsNumber());
	}
	else if (JsonVal->Type == EJson::Boolean)
	{
		ValStr = JsonVal->AsBool() ? TEXT("true") : TEXT("false");
	}
	else if (JsonVal->Type == EJson::Null)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("scalar field cannot be null");
		return;
	}
	else
	{
		ValStr = JsonVal->AsString();
	}

	// Snapshot current value for the report.
	Prop->ExportText_Direct(OutWrite.CurrentValue, ValuePtr, ValuePtr, Owner, PPF_None);
	OutWrite.ProposedValue = ValStr;

	// UserDefinedEnum-in-UserDefinedStruct fields compile to a plain numeric
	// FProperty (no FEnumProperty), so a friendly-name/authored-name token would
	// be rejected by the numeric ImportText. Resolve it to the enum's integer
	// value first; bare-integer tokens return false and fall through unchanged.
	int64 ResolvedEnumValue = 0;
	if (ResolveUserDefinedEnumToken(Prop, ValStr, ResolvedEnumValue))
	{
		if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			NumProp->SetIntPropertyValue(ValuePtr, ResolvedEnumValue);
			OutWrite.bOk = true;
			return;
		}
	}

	// Numeric text the engine import would only partly parse ("abc" -> 0 on a float,
	// "12abc" -> 12 on an int) reports success from ImportText. Check it first so a
	// strict caller refuses it before the value is touched (issue #172).
	if (const FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		if (!MonolithPathWalker_IsCompleteNumericLiteral(NumProp, ValStr))
		{
			const FString Why = FString::Printf(TEXT("'%s' is not a valid %s literal; the engine import would read it as a partial or zero value"),
				*ValStr, *Prop->GetCPPType());
			if (Spec && Spec->bStrict)
			{
				OutWrite.bOk = false;
				OutWrite.Reason = Why + TEXT(" (strict)");
				return;
			}
			if (OutReport)
			{
				FBulkFillFieldWrite Drop = OutWrite;
				Drop.bOk = true;
				Drop.Reason = Why;
				OutReport->SilentDrops.Add(Drop);
			}
		}
	}

	// Per UE 5.7 source_query result: ImportText_Direct(Buffer, Data, OwnerObject, PortFlags, ErrorText)
	// returns nullptr on failure. Capture errors via FStringOutputDevice.
	FStringOutputDevice ErrText;
	const TCHAR* Result = Prop->ImportText_Direct(*ValStr, ValuePtr, Owner, PPF_None, &ErrText);
	if (!Result)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = FString::Printf(TEXT("ImportText rejected '%s'%s%s"),
			*ValStr,
			ErrText.IsEmpty() ? TEXT("") : TEXT(": "),
			*ErrText);
	}
	else
	{
		OutWrite.bOk = true;
	}
}

// ---------------------------------------------------------------------------
// Enum write — per design quirk: enum keys serialise as value-name strings.
// UEnum::GetValueByNameString returns INDEX_NONE on miss (verified Enum.cpp:1046).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteEnum(FEnumProperty* EnumProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite)
{
	const FString NameStr = JsonVal->AsString();
	OutWrite.ProposedValue = NameStr;

	UEnum* Enum = EnumProp->GetEnum();
	if (!Enum)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("enum property has no UEnum metadata");
		return;
	}

	const int64 Value = Enum->GetValueByNameString(NameStr);
	if (Value == INDEX_NONE)
	{
		OutWrite.bOk = false;
		// Build a "did you mean" hint — first 3 enum entries.
		FString Hint;
		const int32 N = FMath::Min(3, Enum->NumEnums() - 1); // -1 for _MAX
		for (int32 i = 0; i < N; ++i)
		{
			Hint += (i == 0 ? TEXT("") : TEXT(", "));
			Hint += Enum->GetNameStringByIndex(i);
		}
		OutWrite.Reason = FString::Printf(TEXT("enum value '%s' not found; valid entries include: %s"),
			*NameStr, *Hint);
		return;
	}

	// Snapshot pre-write value as a name string.
	FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
	if (Underlying)
	{
		const int64 OldValue = Underlying->GetSignedIntPropertyValue(ValuePtr);
		OutWrite.CurrentValue = Enum->GetNameStringByValue(OldValue);
		Underlying->SetIntPropertyValue(ValuePtr, Value);
	}
	OutWrite.bOk = true;
}

// ---------------------------------------------------------------------------
// Soft-object ref — FSoftObjectProperty inherits ImportText from FProperty
// (verified search_source: PropertySoftObjectPtr.cpp:208 ConvertFromType).
// The TSoftObjectPtr value is set via the path string going through the normal
// ImportText_Direct path.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteSoftObjectRef(FSoftObjectProperty* SoftProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, FBulkFillFieldWrite& OutWrite)
{
	const FString PathStr = JsonVal->AsString();
	OutWrite.ProposedValue = PathStr;

	SoftProp->ExportText_Direct(OutWrite.CurrentValue, ValuePtr, ValuePtr, nullptr, PPF_None);

	FStringOutputDevice ErrText;
	const TCHAR* Result = SoftProp->ImportText_Direct(*PathStr, ValuePtr, nullptr, PPF_None, &ErrText);
	OutWrite.bOk = (Result != nullptr);
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("Soft-ref '%s' failed: %s"), *PathStr, *ErrText);
	}
}

// ---------------------------------------------------------------------------
// Hard object ref — StaticLoadObject path. Use the raw form
// SetObjectPropertyValue (verified PropertyBaseObject.cpp:671 via plan §4 H4)
// because we hold ValuePtr directly, not a container offset.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteObjectRef(FObjectProperty* ObjProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* /*Container*/, FBulkFillFieldWrite& OutWrite)
{
	const FString PathStr = JsonVal->AsString();
	OutWrite.ProposedValue = PathStr;

	// Snapshot current pointed-at object's path (or empty if null).
	UObject* OldRef = ObjProp->GetObjectPropertyValue(ValuePtr);
	OutWrite.CurrentValue = OldRef ? OldRef->GetPathName() : FString();

	if (PathStr.IsEmpty())
	{
		ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
		OutWrite.bOk = true;
		return;
	}

	UObject* Resolved = StaticLoadObject(ObjProp->PropertyClass, nullptr, *PathStr);
	if (!Resolved)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = FString::Printf(TEXT("hard ref '%s' did not resolve to a %s"),
			*PathStr, *ObjProp->PropertyClass->GetName());
		return;
	}
	ObjProp->SetObjectPropertyValue(ValuePtr, Resolved);
	OutWrite.bOk = true;
}

// ---------------------------------------------------------------------------
// Array write — FScriptArrayHelper. Per UE 5.7 search_source (plan §4):
// ctor at UnrealType.h:4455, AddUninitializedValues at 4340, AddValue at 4331.
// Per-element dispatch through DispatchByPropertyType so nested types work.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteArray(FArrayProperty* ArrayProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
	if (!JsonVal->TryGetArray(JsonArray) || !JsonArray)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON array");
		return;
	}

	FScriptArrayHelper Helper(ArrayProp, ValuePtr);
	Helper.EmptyValues(JsonArray->Num());
	if (JsonArray->Num() > 0)
	{
		Helper.AddUninitializedValues(JsonArray->Num());
	}

	int32 LocalErrors = 0;
	for (int32 i = 0; i < JsonArray->Num(); ++i)
	{
		uint8* ElemPtr = Helper.GetRawPtr(i);
		// Init each element so ImportText has a stable starting point.
		ArrayProp->Inner->InitializeValue(ElemPtr);

		FBulkFillFieldWrite W;
		W.Path = FString::Printf(TEXT("%s[%d]"), *PathPrefix, i);
		DispatchByPropertyType(ArrayProp->Inner, ElemPtr, (*JsonArray)[i], Owner, Spec, OutReport, W.Path, W);
		if (!W.bOk) { ++LocalErrors; }
		OutReport.FieldWrites.Add(W);
	}

	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("[%d elements]"), JsonArray->Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d element write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Map write — FScriptMapHelper. AddPair clones key+value (verified UnrealType.h:5320).
// JSON shape: object whose string keys are stringified property-keys (FName/FString/int).
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteMap(FMapProperty* MapProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TSharedPtr<FJsonObject>* JsonObj = nullptr;
	if (!JsonVal->TryGetObject(JsonObj) || !JsonObj || !(*JsonObj).IsValid())
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON object for map");
		return;
	}

	FScriptMapHelper Helper(MapProp, ValuePtr);
	Helper.EmptyValues((*JsonObj)->Values.Num());

	int32 LocalErrors = 0;
	int32 Index = 0;
	for (const auto& Pair : (*JsonObj)->Values)
	{
		// Allocate scratch key + value buffers, init them, ImportText key from the
		// JSON object's STRING key, then dispatch the value through the inner walker.
		void* KeyTemp = FMemory::Malloc(MapProp->KeyProp->GetSize(), MapProp->KeyProp->GetMinAlignment());
		void* ValTemp = FMemory::Malloc(MapProp->ValueProp->GetSize(), MapProp->ValueProp->GetMinAlignment());
		MapProp->KeyProp->InitializeValue(KeyTemp);
		MapProp->ValueProp->InitializeValue(ValTemp);

		const FString PairKeyStr = MonolithKeyToString(Pair.Key);
		FBulkFillFieldWrite KeyWrite;
		KeyWrite.Path = FString::Printf(TEXT("%s{key#%d}"), *PathPrefix, Index);
		{
			FStringOutputDevice ErrText;
			const TCHAR* Result = MapProp->KeyProp->ImportText_Direct(*PairKeyStr, KeyTemp, Owner, PPF_None, &ErrText);
			KeyWrite.ProposedValue = PairKeyStr;
			KeyWrite.bOk = (Result != nullptr);
			if (!KeyWrite.bOk)
			{
				KeyWrite.Reason = FString::Printf(TEXT("map key '%s' rejected: %s"), *PairKeyStr, *ErrText);
				++LocalErrors;
			}
		}

		FBulkFillFieldWrite ValWrite;
		ValWrite.Path = FString::Printf(TEXT("%s{val#%d}"), *PathPrefix, Index);
		DispatchByPropertyType(MapProp->ValueProp, ValTemp, Pair.Value, Owner, Spec, OutReport, ValWrite.Path, ValWrite);
		if (!ValWrite.bOk) { ++LocalErrors; }

		if (KeyWrite.bOk && ValWrite.bOk)
		{
			Helper.AddPair(KeyTemp, ValTemp);
		}

		OutReport.FieldWrites.Add(KeyWrite);
		OutReport.FieldWrites.Add(ValWrite);

		// Release scratch (AddPair clones).
		MapProp->KeyProp->DestroyValue(KeyTemp);
		MapProp->ValueProp->DestroyValue(ValTemp);
		FMemory::Free(KeyTemp);
		FMemory::Free(ValTemp);
		++Index;
	}

	Helper.Rehash();
	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("{%d entries}"), (*JsonObj)->Values.Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d map write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Set write — FScriptSetHelper. Per UE 5.7 source_query: ctor at UnrealType.h:5710,
// Rehash() at PropertySet.cpp:1032 (mandatory post-population).
// JSON shape: array of element values.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteSet(FSetProperty* SetProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
	if (!JsonVal->TryGetArray(JsonArray) || !JsonArray)
	{
		OutWrite.bOk = false;
		OutWrite.Reason = TEXT("expected JSON array for set");
		return;
	}

	FScriptSetHelper Helper(SetProp, ValuePtr);
	Helper.EmptyElements(JsonArray->Num());

	int32 LocalErrors = 0;
	for (int32 i = 0; i < JsonArray->Num(); ++i)
	{
		void* ElemTemp = FMemory::Malloc(SetProp->ElementProp->GetSize(), SetProp->ElementProp->GetMinAlignment());
		SetProp->ElementProp->InitializeValue(ElemTemp);

		FBulkFillFieldWrite W;
		W.Path = FString::Printf(TEXT("%s{#%d}"), *PathPrefix, i);
		DispatchByPropertyType(SetProp->ElementProp, ElemTemp, (*JsonArray)[i], Owner, Spec, OutReport, W.Path, W);
		if (!W.bOk) { ++LocalErrors; }
		OutReport.FieldWrites.Add(W);

		if (W.bOk)
		{
			Helper.AddElement(ElemTemp);
		}

		SetProp->ElementProp->DestroyValue(ElemTemp);
		FMemory::Free(ElemTemp);
	}

	Helper.Rehash();
	OutWrite.bOk = (LocalErrors == 0);
	OutWrite.ProposedValue = FString::Printf(TEXT("{%d unique elements}"), JsonArray->Num());
	if (!OutWrite.bOk)
	{
		OutWrite.Reason = FString::Printf(TEXT("%d set write(s) failed"), LocalErrors);
	}
}

// ---------------------------------------------------------------------------
// Struct write — recurse via WriteTree on nested JSON object, or fall through
// to ImportText for "(X=1,Y=2,Z=3)" literal forms.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::WriteStruct(FStructProperty* StructProp, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonVal, UObject* Owner, const FBulkFillSpec& Spec, FDryRunReport& OutReport, const FString& PathPrefix, FBulkFillFieldWrite& OutWrite)
{
	// Object form -> recursive walk into the nested struct's properties.
	if (JsonVal->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (!JsonVal->TryGetObject(NestedObj) || !NestedObj || !(*NestedObj).IsValid())
		{
			OutWrite.bOk = false;
			OutWrite.Reason = TEXT("nested struct expected JSON object");
			return;
		}

		// Per UE57Gotchas.md §JSON: empty-not-null guard.
		if ((*NestedObj)->Values.Num() == 0)
		{
			OutWrite.bOk = true; // empty object = no-op write, not an error
			OutWrite.ProposedValue = TEXT("{}");
			return;
		}

		int32 LocalErrors = 0;
		for (const auto& Pair : (*NestedObj)->Values)
		{
			const FString PairKeyStr = MonolithKeyToString(Pair.Key);
			FBulkFillFieldWrite W;
			W.Path = FString::Printf(TEXT("%s.%s"), *PathPrefix, *PairKeyStr);
			FProperty* InnerProp = FindPropertyForwarding(StructProp->Struct, PairKeyStr);
			if (!InnerProp)
			{
				W.bOk = false;
				W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *PairKeyStr, *StructProp->Struct->GetName());
				OutReport.FieldWrites.Add(W);
				++LocalErrors;
				continue;
			}
			void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
			DispatchByPropertyType(InnerProp, InnerPtr, Pair.Value, Owner, Spec, OutReport, W.Path, W);
			if (!W.bOk) { ++LocalErrors; }
			OutReport.FieldWrites.Add(W);
		}
		OutWrite.bOk = (LocalErrors == 0);
		OutWrite.ProposedValue = FString::Printf(TEXT("{%d fields}"), (*NestedObj)->Values.Num());
		if (!OutWrite.bOk)
		{
			OutWrite.Reason = FString::Printf(TEXT("%d nested write(s) failed"), LocalErrors);
		}
		return;
	}

	// String form -> ImportText literal grammar ("(X=1,Y=2,Z=3)").
	WriteScalar(StructProp, ValuePtr, JsonVal, Owner, OutWrite, &Spec, &OutReport);
}

// ---------------------------------------------------------------------------
// WriteTree — top-level entry. Iterates the JSON object's keys and dispatches
// each into the matching FProperty on Container.
// ---------------------------------------------------------------------------
FDryRunReport FMonolithReflectionWalker::WriteTree(
	const TSharedPtr<FJsonObject>& Tree,
	UStruct* TopStruct,
	void* Container,
	UObject* OwnerForCradle,
	const FBulkFillSpec& Spec)
{
	check(IsInGameThread());

	FDryRunReport Report;
	Report.bWouldApply = true; // optimistic; cleared at the end on strict + errors.

	// Per UE57Gotchas.md §JSON: empty-not-null guard.
	if (!Tree.IsValid() || Tree->Values.Num() == 0)
	{
		Report.bWouldApply = false;
		return Report;
	}
	if (!TopStruct || !Container)
	{
		Report.bWouldApply = false;
		return Report;
	}

	for (const auto& Pair : Tree->Values)
	{
		const FString PairKeyStr = MonolithKeyToString(Pair.Key);
		FBulkFillFieldWrite W;
		W.Path = PairKeyStr;
		FProperty* Prop = FindPropertyForwarding(TopStruct, PairKeyStr);
		if (!Prop)
		{
			W.bOk = false;
			W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *PairKeyStr, *TopStruct->GetName());
			Report.FieldWrites.Add(W);
			continue;
		}
		if (IsDeprecatedProperty(Prop, W.Reason))
		{
			W.bOk = false;
			Report.FieldWrites.Add(W);
			continue;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		DispatchByPropertyType(Prop, ValuePtr, Pair.Value, OwnerForCradle, Spec, Report, PairKeyStr, W);
		Report.FieldWrites.Add(W);
	}

	// Strict-mode handling per Decision Q6.
	Report.Errors = Algo::CountIf(Report.FieldWrites, [](const FBulkFillFieldWrite& Fw){ return !Fw.bOk; });
	if (Spec.bStrict && Report.Errors > 0)
	{
		Report.bWouldApply = false;
	}
	return Report;
}

// ---------------------------------------------------------------------------
// InspectTree — dry-run. Same shape as WriteTree, but ALL writes route through
// a per-field scratch buffer allocated by FProperty::InitializeValue and
// freed by DestroyValue. Container is never mutated.
// Guarantees: test Leviathan.Monolith.Reflection.DryRunNoSideEffects passes.
// ---------------------------------------------------------------------------
FDryRunReport FMonolithReflectionWalker::InspectTree(
	const TSharedPtr<FJsonObject>& Tree,
	UStruct* TopStruct,
	const void* Container,
	const FBulkFillSpec& Spec)
{
	check(IsInGameThread());

	FDryRunReport Report;
	Report.bWouldApply = false; // dry-run NEVER applies

	if (!Tree.IsValid() || Tree->Values.Num() == 0 || !TopStruct)
	{
		return Report;
	}

	for (const auto& Pair : Tree->Values)
	{
		const FString PairKeyStr = MonolithKeyToString(Pair.Key);
		FBulkFillFieldWrite W;
		W.Path = PairKeyStr;
		FProperty* Prop = FindPropertyForwarding(TopStruct, PairKeyStr);
		if (!Prop)
		{
			W.bOk = false;
			W.Reason = FString::Printf(TEXT("unknown field '%s' on %s"), *PairKeyStr, *TopStruct->GetName());
			Report.FieldWrites.Add(W);
			continue;
		}
		if (IsDeprecatedProperty(Prop, W.Reason))
		{
			W.bOk = false;
			Report.FieldWrites.Add(W);
			continue;
		}

		// Allocate a scratch buffer the size of the property; init it from the live
		// value (so the report's `current` is the real value, not the default);
		// dispatch against the scratch; destroy. The real container is never touched.
		void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
		Prop->InitializeValue(Scratch);
		if (Container)
		{
			Prop->CopyCompleteValue(Scratch, Prop->ContainerPtrToValuePtr<void>(Container));
		}
		DispatchByPropertyType(Prop, Scratch, Pair.Value, nullptr, Spec, Report, PairKeyStr, W);
		Prop->DestroyValue(Scratch);
		FMemory::Free(Scratch);

		Report.FieldWrites.Add(W);
	}

	Report.Errors = Algo::CountIf(Report.FieldWrites, [](const FBulkFillFieldWrite& Fw){ return !Fw.bOk; });
	return Report;
}

// ---------------------------------------------------------------------------
// Populate clamp meta from UIMin/UIMax/ClampMin/ClampMax property metadata.
// ---------------------------------------------------------------------------
void FMonolithReflectionWalker::PopulateClampMeta(FProperty* Prop, FSchemaDescriptor& OutDesc)
{
#if WITH_EDITORONLY_DATA
	if (!Prop) return;
	const FString ClampMin = Prop->GetMetaData(TEXT("ClampMin"));
	const FString ClampMax = Prop->GetMetaData(TEXT("ClampMax"));
	const FString UIMin = Prop->GetMetaData(TEXT("UIMin"));
	const FString UIMax = Prop->GetMetaData(TEXT("UIMax"));
	const FString& MinStr = !ClampMin.IsEmpty() ? ClampMin : UIMin;
	const FString& MaxStr = !ClampMax.IsEmpty() ? ClampMax : UIMax;
	if (!MinStr.IsEmpty()) { OutDesc.RangeMin = FCString::Atof(*MinStr); }
	if (!MaxStr.IsEmpty()) { OutDesc.RangeMax = FCString::Atof(*MaxStr); }
#endif
}

// ---------------------------------------------------------------------------
// DescribeStruct — recursive FSchemaDescriptor builder.
// Per design Decision Q3: rich custom tree, NOT JSON Schema standard.
// ---------------------------------------------------------------------------
FSchemaDescriptor FMonolithReflectionWalker::DescribeStruct(UStruct* TopStruct, int32 MaxDepth)
{
	FSchemaDescriptor Root;
	if (!TopStruct || MaxDepth <= 0)
	{
		return Root;
	}
	Root.FieldPath = TopStruct->GetName();
	Root.TypeName = TopStruct->GetName();

	for (TFieldIterator<FProperty> It(TopStruct); It; ++It)
	{
		FProperty* Prop = *It;
		FSchemaDescriptor Child;
		Child.FieldPath = Prop->GetName();
		Child.TypeName = Prop->GetCPPType();
		PopulateClampMeta(Prop, Child);

		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				// Native FEnumProperty: real C++ enum (or UENUM) that already
				// carries its UEnum. Same robust sentinel skip as the UDS path
				// so a missing/non-terminal _MAX never drops a real value.
				const int32 N = Enum->NumEnums();
				for (int32 i = 0; i < N; ++i)
				{
					if (IsAutoMaxSentinel(Enum, i))
					{
						continue;
					}
					Child.EnumValues.Add(Enum->GetNameStringByIndex(i));
				}
			}
			Child.ImportTextForm = (Child.EnumValues.Num() > 0) ? Child.EnumValues[0] : FString();
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Field1=...,Field2=...)");
			if (MaxDepth > 1 && StructProp->Struct)
			{
				Child.Children.Add(DescribeStruct(StructProp->Struct, MaxDepth - 1));
			}
		}
		else if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			// Engine FMapProperty::ImportText is POSITIONAL — ((K,V),(K,V)), NOT
			// ((Key=..,Value=..)). Compose a single-entry example from the key/value
			// property shapes so the hint is copy-pasteable (e.g. a Name->LinearColor
			// map emits (("Text",(R=1.0,G=1.0,B=1.0,A=1.0)))).
			const FString KeyExample = ComposeExampleToken(MapProp->KeyProp, /*Depth=*/2);
			const FString ValueExample = ComposeExampleToken(MapProp->ValueProp, /*Depth=*/2);
			Child.ImportTextForm = FString::Printf(TEXT("((%s,%s))"), *KeyExample, *ValueExample);
			// Optional: descend into value type if it is a struct.
			if (MaxDepth > 1 && MapProp->ValueProp)
			{
				if (FStructProperty* ValueStruct = CastField<FStructProperty>(MapProp->ValueProp))
				{
					if (ValueStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(ValueStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Element1,Element2,Element3)");
			if (MaxDepth > 1 && ArrayProp->Inner)
			{
				if (FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner))
				{
					if (InnerStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(InnerStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("(Element1,Element2)");
			if (MaxDepth > 1 && SetProp->ElementProp)
			{
				if (FStructProperty* ElemStruct = CastField<FStructProperty>(SetProp->ElementProp))
				{
					if (ElemStruct->Struct)
					{
						Child.Children.Add(DescribeStruct(ElemStruct->Struct, MaxDepth - 1));
					}
				}
			}
		}
		else if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("/Game/Path/To/Asset.Asset");
			Child.TypeName = FString::Printf(TEXT("TSoftObjectPtr<%s>"), SoftProp->PropertyClass ? *SoftProp->PropertyClass->GetName() : TEXT("UObject"));
		}
		else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			Child.ImportTextForm = TEXT("/Game/Path/To/Asset.Asset");
			Child.TypeName = FString::Printf(TEXT("%s*"), ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("UObject"));
		}
		else if (UEnum* UdsEnum = RecoverUserDefinedEnum(Prop))
		{
			// UserDefinedEnum field inside a UserDefinedStruct: compiles to a plain
			// numeric FProperty, so the FEnumProperty branch above never fires.
			// Surface the recovered enumerators (friendly display names) and a
			// human type name instead of the bare "int32" CPPType. Iterate EVERY
			// index, skipping only the genuine auto-_MAX sentinel — UserDefinedEnums
			// usually have none, so the old `NumEnums() - 1` dropped the last value.
			const int32 N = UdsEnum->NumEnums();
			for (int32 i = 0; i < N; ++i)
			{
				if (IsAutoMaxSentinel(UdsEnum, i))
				{
					continue;
				}
				Child.EnumValues.Add(UdsEnum->GetDisplayNameTextByIndex(i).ToString());
			}
			Child.TypeName = UdsEnum->GetName();
			Child.ImportTextForm = (Child.EnumValues.Num() > 0) ? Child.EnumValues[0] : FString();
		}
		else
		{
			// Scalar — leave ImportTextForm empty; CPPType already communicates the shape.
		}

		Root.Children.Add(Child);
	}
	return Root;
}
