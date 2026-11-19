// SPDX-License-Identifier: MIT
// FMonolithReflectionWalker property-path automation tests.
//
// Covers ResolvePathWithOptions / InspectLeaf / ListDefaultedStructFields:
//   - issue #172: dry-run `current` on an existing map entry, strict numeric text,
//     create_missing_keys in a dry run, and leaf-only creates of a map entry;
//   - object-array selectors ([@Name], [@class=...]) and the write-side package scope.

#include "Misc/AutomationTest.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithBulkFillTypes.h"
#include "MonolithReflectionPathTestTypes.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithReflectionPathTest
{
	struct FFixture
	{
		FMonolithPathTestRoot Root;
		FGuid ExistingKey;

		FFixture()
		{
			ExistingKey = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
			FMonolithPathTestEntry& Entry = Root.Overrides.Add(ExistingKey);
			Entry.bEnable = true;
			Entry.Value.TypeTag = 3;
			Entry.Value.Float = -0.45f;
			Entry.CachedName = FName(TEXT("Blend Offset"));
		}

		FString KeyPath(const FGuid& Key, const TCHAR* Suffix) const
		{
			return FString::Printf(TEXT("Overrides[%s]%s"), *Key.ToString(EGuidFormats::Digits), Suffix);
		}
	};

	TSharedPtr<FJsonValue> Str(const TCHAR* Text)
	{
		return MakeShared<FJsonValueString>(FString(Text));
	}
}

// ---------------------------------------------------------------------------
// Dry run reads the live entry, not a default value (issue #172 trap 1).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionPathDryRunCurrentTest,
	"Leviathan.Monolith.Reflection.Path.DryRunReportsLiveMapEntryValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionPathDryRunCurrentTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithReflectionPathTest;
	FFixture F;
	UScriptStruct* Struct = FMonolithPathTestRoot::StaticStruct();

	FMonolithReflectionWalker::FPathResolveOptions Options;
	Options.bNoMutation = true;

	struct FCase { const TCHAR* Suffix; const TCHAR* Value; };
	const FCase Cases[] = {
		{ TEXT(".Value.Float"), TEXT("-0.3") },
		{ TEXT(".bEnable"), TEXT("false") },
		{ TEXT(".CachedName"), TEXT("Other") },
	};
	TArray<FString> Expected;
	{
		const FMonolithPathTestEntry& Entry = F.Root.Overrides[F.ExistingKey];
		auto ExportField = [](const UStruct* Owner, const TCHAR* Name, const void* ContainerPtr)
		{
			FString S;
			const FProperty* Prop = FindFProperty<FProperty>(Owner, Name);
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ContainerPtr);
			Prop->ExportText_Direct(S, ValuePtr, ValuePtr, nullptr, PPF_None);
			return S;
		};
		Expected.Add(ExportField(FMonolithPathTestUnionValue::StaticStruct(), TEXT("Float"), &Entry.Value));
		Expected.Add(ExportField(FMonolithPathTestEntry::StaticStruct(), TEXT("bEnable"), &Entry));
		Expected.Add(ExportField(FMonolithPathTestEntry::StaticStruct(), TEXT("CachedName"), &Entry));
	}

	for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(Cases)); ++i)
	{
		const FString Path = F.KeyPath(F.ExistingKey, Cases[i].Suffix);
		const FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, Path, Options);
		if (!TestTrue(FString::Printf(TEXT("resolve %s (%s)"), *Path, *Resolved.Error), Resolved.bOk))
		{
			continue;
		}
		TestFalse(TEXT("existing key is not resolved into scratch"), Resolved.bLeafInScratch);

		FBulkFillSpec Spec;
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(Resolved, Str(Cases[i].Value), nullptr, Spec, Report, Path);
		TestTrue(FString::Printf(TEXT("inspect %s ok"), *Path), W.bOk);
		TestTrue(FString::Printf(TEXT("current of %s is the live value (got '%s', want '%s')"), *Path, *W.CurrentValue, *Expected[i]),
			W.CurrentValue == Expected[i]);
	}

	const FMonolithPathTestEntry& After = F.Root.Overrides[F.ExistingKey];
	TestEqual(TEXT("dry run left Float unchanged"), After.Value.Float, -0.45f);
	TestTrue(TEXT("dry run left bEnable unchanged"), After.bEnable);
	TestEqual(TEXT("dry run left CachedName unchanged"), After.CachedName, FName(TEXT("Blend Offset")));
	return true;
}

// ---------------------------------------------------------------------------
// Strict numeric text check (issue #172 trap 2).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionPathStrictNumericTest,
	"Leviathan.Monolith.Reflection.Path.StrictRejectsNonNumericText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionPathStrictNumericTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithReflectionPathTest;
	FFixture F;
	UScriptStruct* Struct = FMonolithPathTestRoot::StaticStruct();
	const FString FloatPath = F.KeyPath(F.ExistingKey, TEXT(".Value.Float"));
	const FString IntPath = F.KeyPath(F.ExistingKey, TEXT(".Value.TypeTag"));

	FMonolithReflectionWalker::FPathResolveOptions Options;
	Options.bNoMutation = true;
	const FMonolithReflectionWalker::FPathResolveResult FloatLeaf =
		FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, FloatPath, Options);
	const FMonolithReflectionWalker::FPathResolveResult IntLeaf =
		FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, IntPath, Options);
	if (!TestTrue(TEXT("float leaf resolves"), FloatLeaf.bOk) || !TestTrue(TEXT("int leaf resolves"), IntLeaf.bOk))
	{
		return false;
	}

	FBulkFillSpec Strict;
	Strict.bStrict = true;
	FBulkFillSpec Lenient;

	{
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(FloatLeaf, Str(TEXT("abc")), nullptr, Strict, Report, FloatPath);
		TestFalse(TEXT("strict float rejects 'abc'"), W.bOk);
	}
	{
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(IntLeaf, Str(TEXT("12abc")), nullptr, Strict, Report, IntPath);
		TestFalse(TEXT("strict int rejects '12abc'"), W.bOk);
	}
	{
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(FloatLeaf, Str(TEXT("abc")), nullptr, Lenient, Report, FloatPath);
		TestTrue(TEXT("non-strict float keeps accepting 'abc'"), W.bOk);
		TestEqual(TEXT("non-strict float reports a silent drop"), Report.SilentDrops.Num(), 1);
	}
	const TCHAR* GoodFloats[] = { TEXT("-0.3"), TEXT("1"), TEXT(".5"), TEXT("1.5e-3"), TEXT("2f"), TEXT(" 4.0 ") };
	for (const TCHAR* Good : GoodFloats)
	{
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(FloatLeaf, Str(Good), nullptr, Strict, Report, FloatPath);
		TestTrue(FString::Printf(TEXT("strict float accepts '%s'"), Good), W.bOk);
	}
	{
		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(FloatLeaf, MakeShared<FJsonValueNumber>(0.25), nullptr, Strict, Report, FloatPath);
		TestTrue(TEXT("strict float accepts a JSON number"), W.bOk);
	}
	TestEqual(TEXT("inspections left the live value unchanged"), F.Root.Overrides[F.ExistingKey].Value.Float, -0.45f);
	return true;
}

// ---------------------------------------------------------------------------
// Dry run honours create_missing_keys without touching the map (issue #172 trap 3).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionPathDryRunCreateKeyTest,
	"Leviathan.Monolith.Reflection.Path.DryRunHonoursCreateMissingKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionPathDryRunCreateKeyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithReflectionPathTest;
	FFixture F;
	UScriptStruct* Struct = FMonolithPathTestRoot::StaticStruct();
	const FGuid NewKey(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
	const FString Path = F.KeyPath(NewKey, TEXT(".Value.Float"));

	FMonolithReflectionWalker::FPathResolveOptions Options;
	Options.bNoMutation = true;

	const FMonolithReflectionWalker::FPathResolveResult NoCreate =
		FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, Path, Options);
	TestFalse(TEXT("missing key without create_missing_keys fails"), NoCreate.bOk);

	Options.bCreateMissingKeys = true;
	const FMonolithReflectionWalker::FPathResolveResult Resolved =
		FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, Path, Options);
	if (!TestTrue(FString::Printf(TEXT("dry resolve with create_missing_keys (%s)"), *Resolved.Error), Resolved.bOk))
	{
		return false;
	}
	TestTrue(TEXT("reports the created key"), Resolved.bCreatedMissingKey);
	TestTrue(TEXT("leaf lives in scratch"), Resolved.bLeafInScratch);
	TestFalse(TEXT("leaf is below the created entry"), Resolved.bLeafIsCreatedElement);
	TestEqual(TEXT("live map not grown"), F.Root.Overrides.Num(), 1);

	FBulkFillSpec Spec;
	FDryRunReport Report;
	const FBulkFillFieldWrite W = FMonolithReflectionWalker::InspectLeaf(Resolved, Str(TEXT("1.0")), nullptr, Spec, Report, Path);
	TestTrue(TEXT("scratch write ok"), W.bOk);
	const TArray<FString> Defaulted = FMonolithReflectionWalker::ListDefaultedStructFields(Resolved.CreatedElementProp, Resolved.CreatedElementPtr);
	TestTrue(TEXT("dry run lists bEnable as defaulted"), Defaulted.Contains(TEXT("bEnable")));
	TestTrue(TEXT("dry run lists Value.TypeTag as defaulted"), Defaulted.Contains(TEXT("Value.TypeTag")));
	TestFalse(TEXT("dry run does not list the written leaf"), Defaulted.Contains(TEXT("Value.Float")));
	TestEqual(TEXT("live map still not grown"), F.Root.Overrides.Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// Leaf-only create reports defaulted siblings; new_element_value seeds the entry
// (issue #172 trap 4).
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionPathLeafOnlyCreateTest,
	"Leviathan.Monolith.Reflection.Path.LeafOnlyCreateReportsDefaultedFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionPathLeafOnlyCreateTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithReflectionPathTest;
	UScriptStruct* Struct = FMonolithPathTestRoot::StaticStruct();
	FBulkFillSpec Spec;

	// Leaf-only create.
	{
		FFixture F;
		const FGuid NewKey(1, 2, 3, 4);
		const FString Path = F.KeyPath(NewKey, TEXT(".Value.Float"));
		FMonolithReflectionWalker::FPathResolveOptions Options;
		Options.bCreateMissingKeys = true;
		const FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, Path, Options);
		if (!TestTrue(FString::Printf(TEXT("create resolve (%s)"), *Resolved.Error), Resolved.bOk))
		{
			return false;
		}
		TestTrue(TEXT("key created"), Resolved.bCreatedMissingKey);
		TestFalse(TEXT("real create is not scratch"), Resolved.bLeafInScratch);
		TestEqual(TEXT("map grew"), F.Root.Overrides.Num(), 2);

		FDryRunReport Report;
		const FBulkFillFieldWrite W = FMonolithReflectionWalker::WriteLeaf(Resolved.LeafProp, Resolved.LeafPtr, Str(TEXT("1.0")), nullptr, Spec, Report, Path);
		TestTrue(TEXT("leaf write ok"), W.bOk);
		TestEqual(TEXT("leaf written into the new entry"), F.Root.Overrides[NewKey].Value.Float, 1.0f);

		const TArray<FString> Defaulted = FMonolithReflectionWalker::ListDefaultedStructFields(Resolved.CreatedElementProp, Resolved.CreatedElementPtr);
		TestTrue(TEXT("bEnable defaulted"), Defaulted.Contains(TEXT("bEnable")));
		TestTrue(TEXT("Value.TypeTag defaulted"), Defaulted.Contains(TEXT("Value.TypeTag")));
		TestTrue(TEXT("Value.Label defaulted"), Defaulted.Contains(TEXT("Value.Label")));
		TestTrue(TEXT("CachedName defaulted"), Defaulted.Contains(TEXT("CachedName")));
		TestFalse(TEXT("Value.Float not defaulted"), Defaulted.Contains(TEXT("Value.Float")));
		TestFalse(TEXT("Value itself not listed (it differs)"), Defaulted.Contains(TEXT("Value")));
	}

	// Create seeded from new_element_value.
	{
		FFixture F;
		const FGuid NewKey(5, 6, 7, 8);
		const FString Path = F.KeyPath(NewKey, TEXT(".Value.Float"));
		FMonolithReflectionWalker::FPathResolveOptions Options;
		Options.bCreateMissingKeys = true;
		Options.NewElementValue = Str(TEXT("(bEnable=True,Value=(TypeTag=2),CachedName=\"Seeded\")"));
		const FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, Path, Options);
		if (!TestTrue(FString::Printf(TEXT("seeded create resolve (%s)"), *Resolved.Error), Resolved.bOk))
		{
			return false;
		}
		TestTrue(TEXT("new_element_value applied"), Resolved.bAppliedNewElementValue);

		FDryRunReport Report;
		FMonolithReflectionWalker::WriteLeaf(Resolved.LeafProp, Resolved.LeafPtr, Str(TEXT("1.0")), nullptr, Spec, Report, Path);
		const FMonolithPathTestEntry& Entry = F.Root.Overrides[NewKey];
		TestTrue(TEXT("seeded bEnable"), Entry.bEnable);
		TestEqual(TEXT("seeded TypeTag"), Entry.Value.TypeTag, 2);
		TestEqual(TEXT("seeded CachedName"), Entry.CachedName, FName(TEXT("Seeded")));
		TestEqual(TEXT("leaf written after the seed"), Entry.Value.Float, 1.0f);

		const TArray<FString> Defaulted = FMonolithReflectionWalker::ListDefaultedStructFields(Resolved.CreatedElementProp, Resolved.CreatedElementPtr);
		TestFalse(TEXT("seeded bEnable not defaulted"), Defaulted.Contains(TEXT("bEnable")));
		TestFalse(TEXT("seeded TypeTag not defaulted"), Defaulted.Contains(TEXT("Value.TypeTag")));
		TestTrue(TEXT("unseeded Label still defaulted"), Defaulted.Contains(TEXT("Value.Label")));
	}

	// Bad new_element_value fails the resolve before the map is touched.
	{
		FFixture F;
		FMonolithReflectionWalker::FPathResolveOptions Options;
		Options.bCreateMissingKeys = true;
		Options.NewElementValue = Str(TEXT("notastruct"));
		const FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, F.KeyPath(FGuid(9, 9, 9, 9), TEXT(".Value.Float")), Options);
		TestFalse(TEXT("malformed new_element_value fails"), Resolved.bOk);
		TestEqual(TEXT("map untouched after a rejected seed"), F.Root.Overrides.Num(), 1);
	}

	// new_element_value on an existing key is ignored with a warning.
	{
		FFixture F;
		FMonolithReflectionWalker::FPathResolveOptions Options;
		Options.bCreateMissingKeys = true;
		Options.NewElementValue = Str(TEXT("(bEnable=False)"));
		const FMonolithReflectionWalker::FPathResolveResult Resolved =
			FMonolithReflectionWalker::ResolvePathWithOptions(Struct, &F.Root, F.KeyPath(F.ExistingKey, TEXT(".Value.Float")), Options);
		TestTrue(TEXT("existing key resolves"), Resolved.bOk);
		TestFalse(TEXT("no key created"), Resolved.bCreatedMissingKey);
		TestEqual(TEXT("ignored-seed warning"), Resolved.Warnings.Num(), 1);
		TestTrue(TEXT("existing entry untouched"), F.Root.Overrides[F.ExistingKey].bEnable);
	}
	return true;
}

// ---------------------------------------------------------------------------
// Object-array selectors and the write-side package scope.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithReflectionPathObjectSelectorTest,
	"Leviathan.Monolith.Reflection.Path.ObjectArraySelectorsAndPackageScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReflectionPathObjectSelectorTest::RunTest(const FString& /*Parameters*/)
{
	UPackage* HomePkg = CreatePackage(TEXT("/Temp/MonolithPathTest_Home"));
	UPackage* OtherPkg = CreatePackage(TEXT("/Temp/MonolithPathTest_Other"));
	HomePkg->SetFlags(RF_Transient);
	OtherPkg->SetFlags(RF_Transient);

	// Unique graph names so reruns never collide; node names are unique per graph.
	UEdGraph* Graph = NewObject<UEdGraph>(HomePkg, MakeUniqueObjectName(HomePkg, UEdGraph::StaticClass(), TEXT("PathTestGraph")), RF_Transient);
	UEdGraph* OtherGraph = NewObject<UEdGraph>(OtherPkg, MakeUniqueObjectName(OtherPkg, UEdGraph::StaticClass(), TEXT("PathTestOtherGraph")), RF_Transient);
	UEdGraphNode* NodeA = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("PathTestNode_A"), RF_Transient);
	UEdGraphNode* NodeB = NewObject<UEdGraphNode>(Graph, UEdGraphNode::StaticClass(), TEXT("PathTestNode_B"), RF_Transient);
	UEdGraphNode* Foreign = NewObject<UEdGraphNode>(OtherGraph, UEdGraphNode::StaticClass(), TEXT("PathTestNode_Foreign"), RF_Transient);
	Graph->Nodes.Add(NodeA);
	Graph->Nodes.Add(NodeB);
	Graph->Nodes.Add(Foreign);
	NodeB->NodePosX = 42;

	UClass* GraphClass = Graph->GetClass();
	FMonolithReflectionWalker::FPathResolveOptions Options;
	Options.RootObject = Graph;
	Options.HomePackage = HomePkg;
	Options.bRefuseExternalObjects = true;

	// [@Name] selects by object name (case-insensitive) and reports the node as owner.
	{
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(GraphClass, Graph, TEXT("Nodes[@pathtestnode_b].NodePosX"), Options);
		if (TestTrue(FString::Printf(TEXT("name selector resolves (%s)"), *R.Error), R.bOk))
		{
			TestEqual(TEXT("name selector picked NodeB"), *static_cast<int32*>(R.LeafPtr), 42);
			TestTrue(TEXT("leaf owner is the node"), R.LeafOwnerObject == NodeB);
			TestTrue(TEXT("leaf owner head prop is NodePosX"), R.LeafOwnerHeadProp && R.LeafOwnerHeadProp->GetFName() == TEXT("NodePosX"));
		}
	}
	// Unknown name fails with the element list.
	{
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(GraphClass, Graph, TEXT("Nodes[@Missing].NodePosX"), Options);
		TestFalse(TEXT("unknown name fails"), R.bOk);
		TestTrue(TEXT("error lists elements"), R.Error.Contains(TEXT("PathTestNode_A")));
	}
	// [@class=...] is ambiguous with three nodes of one class.
	{
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(GraphClass, Graph, TEXT("Nodes[@class=EdGraphNode].NodePosX"), Options);
		TestFalse(TEXT("ambiguous class selector fails"), R.bOk);
	}
	// Index path into a node in another package is refused for writes...
	{
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(GraphClass, Graph, TEXT("Nodes[2].NodePosX"), Options);
		TestFalse(TEXT("external node refused"), R.bOk);
		TestTrue(TEXT("refusal names allow_external"), R.Error.Contains(TEXT("allow_external")));
	}
	// ...and followed (but listed) when refusal is off.
	{
		FMonolithReflectionWalker::FPathResolveOptions ReadOptions = Options;
		ReadOptions.bRefuseExternalObjects = false;
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(GraphClass, Graph, TEXT("Nodes[@PathTestNode_Foreign].NodePosX"), ReadOptions);
		TestTrue(TEXT("external node followed for reads"), R.bOk);
		TestEqual(TEXT("external object listed"), R.ExternalObjects.Num(), 1);
	}
	// '@' has no selector meaning on maps: the token goes through the key grammar.
	{
		FMonolithPathTestRoot Dummy;
		const FMonolithReflectionWalker::FPathResolveResult R =
			FMonolithReflectionWalker::ResolvePathWithOptions(FMonolithPathTestRoot::StaticStruct(), &Dummy, TEXT("Overrides[@x].bEnable"), FMonolithReflectionWalker::FPathResolveOptions());
		TestFalse(TEXT("map key '@x' is rejected by the GUID key grammar"), R.bOk);
	}

	Graph->Nodes.Reset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
