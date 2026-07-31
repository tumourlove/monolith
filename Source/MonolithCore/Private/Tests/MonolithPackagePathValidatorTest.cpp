// SPDX-License-Identifier: MIT
// Automation tests for MonolithCore::ValidatePackagePath.
//
// ValidatePackagePath is a pure function, so these tests create no assets, need
// no EditorScriptingUtilities, and leave nothing behind in a user's project.
//
// The PASS table carries the weight here: wiring the validator into the asset
// creation handlers made OVER-rejection the live regression risk, so every path
// shape that used to reach CreatePackage must still be accepted.

#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithPackagePathValidator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithPackagePathTestDetail
{
	struct FPassCase
	{
		const TCHAR* Input;
		const TCHAR* ExpectedNormalized;
		const TCHAR* Why;
	};

	struct FFailCase
	{
		const TCHAR* Input;
		const TCHAR* Why;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCorePackagePathTest,
	"Monolith.Core.PackagePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCorePackagePathTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithPackagePathTestDetail;

	// -----------------------------------------------------------------------
	// PASS: paths the handlers must keep accepting.
	// -----------------------------------------------------------------------
	const FPassCase PassCases[] =
	{
		{ TEXT("/Game/Foo"),                 TEXT("/Game/Foo"),                 TEXT("shallowest game path") },
		{ TEXT("/Game/Foo/Bar/Baz"),         TEXT("/Game/Foo/Bar/Baz"),         TEXT("nested") },
		{ TEXT("/Game/A/B/C/D/E/F/G/H"),     TEXT("/Game/A/B/C/D/E/F/G/H"),     TEXT("deep nesting") },
		{ TEXT("/Game/My_Folder/M_Asset_01"),TEXT("/Game/My_Folder/M_Asset_01"),TEXT("underscores and digits") },
		{ TEXT("/Game/123/456"),             TEXT("/Game/123/456"),             TEXT("digit-only segments") },
		{ TEXT("/Game/Foo/M-Bar"),           TEXT("/Game/Foo/M-Bar"),           TEXT("hyphen is a legal package char") },
		{ TEXT("/Engine/Foo"),               TEXT("/Engine/Foo"),               TEXT("engine root is writable, not read-only") },
		{ TEXT("/Engine/Foo/Bar/Baz"),       TEXT("/Engine/Foo/Bar/Baz"),       TEXT("nested engine path") },

		// Object-path forms newly accepted by the normalisation step. The suffix
		// after the dot exactly names the leaf, so the intent is unambiguous.
		{ TEXT("/Game/Foo/M_Bar.M_Bar"),     TEXT("/Game/Foo/M_Bar"),           TEXT("object path -> package path") },
		{ TEXT("/Game/M_Bar.M_Bar"),         TEXT("/Game/M_Bar"),               TEXT("shallow object path") },
		{ TEXT("/Game/A/B/C/D_1.D_1"),       TEXT("/Game/A/B/C/D_1"),           TEXT("deep object path") },
		{ TEXT("/Game/Foo/M_Bar.m_bar"),     TEXT("/Game/Foo/M_Bar"),           TEXT("leaf match is case-insensitive, like FName") },
		{ TEXT("/Engine/Foo/Bar.Bar"),       TEXT("/Engine/Foo/Bar"),           TEXT("object path under the engine root") },
	};

	for (const FPassCase& Case : PassCases)
	{
		FString Path = Case.Input;
		const FString Error = MonolithCore::ValidatePackagePath(Path);

		TestTrue(
			FString::Printf(TEXT("PASS '%s' (%s) accepted [error='%s']"), Case.Input, Case.Why, *Error),
			Error.IsEmpty());
		TestEqual(
			FString::Printf(TEXT("PASS '%s' (%s) normalised form"), Case.Input, Case.Why),
			Path, FString(Case.ExpectedNormalized));
	}

	// A mounted plugin content root must validate too. Discovered at runtime
	// rather than hard-coded, so the test does not depend on which plugins the
	// host project enables.
	{
		TArray<FString> RootContentPaths;
		FPackageName::QueryRootContentPaths(
			RootContentPaths,
			/*bIncludeReadOnlyRoots=*/false,
			/*bWithoutLeadingSlashes=*/false,
			/*bWithoutTrailingSlashes=*/false);

		FString PluginRoot;
		for (const FString& Root : RootContentPaths)
		{
			if (Root != TEXT("/Game/") && Root != TEXT("/Engine/"))
			{
				PluginRoot = Root;
				break;
			}
		}

		if (PluginRoot.IsEmpty())
		{
			AddWarning(TEXT("Skipping plugin-mount-point case: only /Game/ and /Engine/ are mounted in this run"));
		}
		else
		{
			FString Path = PluginRoot + TEXT("MonolithPathCase/Asset_01");
			const FString Expected = Path;
			const FString Error = MonolithCore::ValidatePackagePath(Path);

			TestTrue(
				FString::Printf(TEXT("PASS plugin root '%s' accepted [error='%s']"), *PluginRoot, *Error),
				Error.IsEmpty());
			TestEqual(TEXT("PASS plugin root path is unchanged"), Path, Expected);

			// Same root, object-path form.
			FString ObjectPath = PluginRoot + TEXT("MonolithPathCase/Asset_01.Asset_01");
			const FString ObjectError = MonolithCore::ValidatePackagePath(ObjectPath);
			TestTrue(
				FString::Printf(TEXT("PASS plugin root object path accepted [error='%s']"), *ObjectError),
				ObjectError.IsEmpty());
			TestEqual(TEXT("PASS plugin root object path normalised"), ObjectPath, Expected);

			AddInfo(FString::Printf(TEXT("Plugin mount point under test: %s"), *PluginRoot));
		}
	}

	// -----------------------------------------------------------------------
	// FAIL: malformed paths, read-only roots, and the ambiguous dot forms that
	// must NOT be normalised.
	// -----------------------------------------------------------------------
	const FFailCase FailCases[] =
	{
		{ TEXT(""),                            TEXT("empty") },
		{ TEXT("Game/Foo"),                    TEXT("no leading slash") },
		{ TEXT("//Game/Foo"),                  TEXT("double leading slash -- the original CreatePackage assert") },
		{ TEXT("/Game/Foo//Bar"),              TEXT("embedded double slash") },
		{ TEXT("/Game/Foo/"),                  TEXT("trailing slash") },
		{ TEXT("/Game/Foo Bar"),               TEXT("embedded space") },
		{ TEXT("/Game/Foo&Bar"),               TEXT("ampersand") },
		{ TEXT("/Game/Foo'Bar"),               TEXT("apostrophe") },
		{ TEXT("/Game/Foo,Bar"),               TEXT("comma") },
		{ TEXT("/Game/Foo#Bar"),               TEXT("hash") },
		{ TEXT("/Script/Foo"),                 TEXT("read-only root") },
		{ TEXT("/Temp/Foo"),                   TEXT("read-only root") },
		{ TEXT("/Config/Foo"),                 TEXT("read-only root") },
		{ TEXT("/MonolithNotAMountPoint/Foo"), TEXT("unmounted root") },

		// Ambiguous dot forms. Normalisation is deliberately conservative, so
		// each of these keeps its dot and is rejected as an invalid character.
		{ TEXT("/Game/Foo/M_Bar.SomethingElse"), TEXT("mismatched leaf -- a real sub-object path") },
		{ TEXT("/Game/Foo/M_Bar.M_Bar.M_Bar"),   TEXT("more than one dot") },
		{ TEXT("/Game/Foo.Bar/M_Baz"),           TEXT("dot inside a folder segment") },
		{ TEXT("/Game/Foo/M_Bar."),              TEXT("trailing dot, empty object name") },
		{ TEXT("/Game/Foo/.M_Bar"),              TEXT("empty leaf before the dot") },
		{ TEXT("/Script/Foo.Foo"),               TEXT("normalises, but the root is still read-only") },
	};

	for (const FFailCase& Case : FailCases)
	{
		FString Path = Case.Input;
		const FString Error = MonolithCore::ValidatePackagePath(Path);

		TestFalse(
			FString::Printf(TEXT("FAIL '%s' (%s) returns a non-empty error"), Case.Input, Case.Why),
			Error.IsEmpty());
		TestEqual(
			FString::Printf(TEXT("FAIL '%s' (%s) leaves the caller's path untouched"), Case.Input, Case.Why),
			Path, FString(Case.Input));
	}

	// -----------------------------------------------------------------------
	// Two-argument overload: OutNormalizedPath is written on success only.
	// -----------------------------------------------------------------------
	{
		FString Out = TEXT("<sentinel>");
		const FString Error = MonolithCore::ValidatePackagePath(FString(TEXT("/Game/Foo Bar")), Out);
		TestFalse(TEXT("two-arg overload reports the error"), Error.IsEmpty());
		TestEqual(TEXT("two-arg overload leaves OutNormalizedPath untouched on failure"),
			Out, FString(TEXT("<sentinel>")));
	}
	{
		FString Out;
		const FString Error = MonolithCore::ValidatePackagePath(FString(TEXT("/Game/Foo/M_Bar.M_Bar")), Out);
		TestTrue(FString::Printf(TEXT("two-arg overload accepts object path [error='%s']"), *Error),
			Error.IsEmpty());
		TestEqual(TEXT("two-arg overload writes the normalised path on success"),
			Out, FString(TEXT("/Game/Foo/M_Bar")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
