// SPDX-License-Identifier: MIT
// FMonolithDryRunGuard — RAII-ish helper used inside existing write actions
// to add `dry_run: true` support with minimal boilerplate. Phase 0 primitive.

#pragma once

#include "CoreMinimal.h"
#include "MonolithBulkFillTypes.h"

struct FMonolithActionResult;

/**
 * Helper used inside existing write actions to add `dry_run: true` support
 * with minimal boilerplate.
 *
 * Usage:
 *   FMonolithActionResult FMyActions::HandleFoo(const TSharedPtr<FJsonObject>& Params)
 *   {
 *       FMonolithDryRunGuard Guard(Params);
 *       // ... do all validation ...
 *       if (Guard.IsDryRun()) { return Guard.MakeDryRunResponse(MyReport); }
 *       // ... commit ...
 *   }
 */
class MONOLITHCORE_API FMonolithDryRunGuard
{
public:
	explicit FMonolithDryRunGuard(const TSharedPtr<FJsonObject>& Params);

	bool IsDryRun() const { return bDryRun; }
	bool IsStrict() const { return bStrict; }

	/** Build a success-shaped JSON-RPC response carrying the report payload. */
	FMonolithActionResult MakeDryRunResponse(const FDryRunReport& Report) const;

	/** Convert a report into a JSON object (extracted for unit-testability). */
	static TSharedPtr<FJsonObject> ReportToJson(const FDryRunReport& Report);

private:
	bool bDryRun = false;
	bool bStrict = false;
};
