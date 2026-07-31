#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

/**
 * Defensive validator for long package paths (e.g. "/Game/Foo/Bar").
 *
 * Added in response to a fatal editor crash where raw JSON input containing
 * "//Game/..." (double leading slash) was passed directly to CreatePackage().
 * CreatePackage asserts on such inputs in UObjectGlobals.cpp:1012. This
 * wrapper converts the assertion into a recoverable error return.
 *
 * It ALSO normalises the object-path form ("/Game/Foo/M_Bar.M_Bar") down to the
 * package path ("/Game/Foo/M_Bar") before validating. INVALID_LONGPACKAGE_CHARACTERS
 * (NameTypes.h) includes '.', so without that step an object path -- exactly what
 * a client copies out of a search or Asset Registry response -- would hard-error
 * on every asset-creation action.
 *
 * Scope note: routing this validator into CreatePackage call sites is
 * incremental, so grep the Source tree for ValidatePackagePath to see which
 * sites currently own it.
 */
namespace MonolithCore
{
	/**
	 * Rewrites "<PackagePath>.<ObjectName>" to "<PackagePath>" when <ObjectName>
	 * is exactly the leaf name of <PackagePath>.
	 *
	 * Deliberately conservative -- it fires only on the unambiguous same-name
	 * case, which is the only form where the caller's intent is certain:
	 *   "/Game/Foo/M_Bar.M_Bar"         -> "/Game/Foo/M_Bar"  (normalised)
	 *   "/Game/Foo/M_Bar.SomethingElse" -> left alone (a real sub-object path)
	 *   "/Game/Foo.Bar/M_Baz"           -> left alone (dot inside a folder segment)
	 *   "/Game/Foo/M_Bar.M_Bar.M_Bar"   -> left alone (more than one dot)
	 *
	 * The leaf comparison is case-insensitive to match FName semantics: an object
	 * named "m_bar" and one named "M_Bar" cannot coexist in the same package, so
	 * a case difference is not an ambiguity.
	 *
	 * @param InPath          Candidate path.
	 * @param OutPackagePath  Receives the package path on success; untouched otherwise.
	 * @return                true if InPath was an unambiguous same-name object path.
	 */
	inline bool TryStripRedundantObjectSuffix(const FString& InPath, FString& OutPackagePath)
	{
		int32 FirstDot = INDEX_NONE;
		if (!InPath.FindChar(TEXT('.'), FirstDot))
		{
			return false;
		}

		// Exactly one dot -- anything else is ambiguous.
		int32 LastDot = INDEX_NONE;
		if (!InPath.FindLastChar(TEXT('.'), LastDot) || LastDot != FirstDot)
		{
			return false;
		}

		const FString PackagePart = InPath.Left(FirstDot);
		const FString ObjectPart = InPath.Mid(FirstDot + 1);
		if (PackagePart.IsEmpty() || ObjectPart.IsEmpty())
		{
			return false;
		}

		// A '/' after the dot means the dot lived inside a folder segment.
		if (ObjectPart.Contains(TEXT("/")))
		{
			return false;
		}

		int32 LastSlash = INDEX_NONE;
		if (!PackagePart.FindLastChar(TEXT('/'), LastSlash))
		{
			return false;
		}

		const FString LeafName = PackagePart.Mid(LastSlash + 1);
		if (LeafName.IsEmpty() || !LeafName.Equals(ObjectPart, ESearchCase::IgnoreCase))
		{
			return false;
		}

		OutPackagePath = PackagePart;
		return true;
	}

	/**
	 * Validates a long package path, tolerating the same-name object-path form.
	 *
	 * @param InPath             Package path to check, e.g. "/Game/Foo/Bar".
	 * @param OutNormalizedPath  On success, receives the path the caller must hand
	 *                           to CreatePackage (redundant object-path suffix
	 *                           stripped, if any). Left untouched on failure, so a
	 *                           caller's own error messages keep quoting raw input.
	 * @return                   Empty FString on success; human-readable error message on failure.
	 */
	inline FString ValidatePackagePath(const FString& InPath, FString& OutNormalizedPath)
	{
		if (InPath.IsEmpty())
		{
			return TEXT("Package path is empty");
		}

		FString Candidate;
		const bool bStrippedSuffix = TryStripRedundantObjectSuffix(InPath, Candidate);
		if (!bStrippedSuffix)
		{
			Candidate = InPath;
		}

		FText OutReason;
		if (!FPackageName::IsValidLongPackageName(Candidate, /*bIncludeReadOnlyRoots=*/false, &OutReason))
		{
			FString Error = FString::Printf(TEXT("Invalid package path '%s': %s"), *InPath, *OutReason.ToString());
			if (!bStrippedSuffix && Candidate.Contains(TEXT(".")))
			{
				// Most likely cause of a '.' reaching here: a sub-object path, or a
				// mistyped asset name after the dot. Say so rather than leaving the
				// caller to decode "invalid character".
				Error += TEXT(" (an object path is accepted only when the name after the '.' matches the asset name, e.g. '/Game/Foo/M_Bar.M_Bar')");
			}
			return Error;
		}

		OutNormalizedPath = Candidate;
		return FString();
	}

	/**
	 * In-place overload. On success InOutPath is rewritten to the normalised
	 * package path, so an existing call site keeps using its own variable for
	 * CreatePackage and gains object-path tolerance without further edits.
	 * On failure InOutPath is left exactly as the caller supplied it.
	 *
	 * There is deliberately NO single-argument const overload: passing a const
	 * string is a compile error rather than a silent "validated but not
	 * normalised" path, which would leave the raw object path going to
	 * CreatePackage. Const callers use the two-argument form above.
	 */
	inline FString ValidatePackagePath(FString& InOutPath)
	{
		FString NormalizedPath;
		const FString Error = ValidatePackagePath(InOutPath, NormalizedPath);
		if (Error.IsEmpty())
		{
			InOutPath = MoveTemp(NormalizedPath);
		}
		return Error;
	}
}
