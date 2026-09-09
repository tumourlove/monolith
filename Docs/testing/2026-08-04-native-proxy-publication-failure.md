# Native Proxy Publication Failure Verification

**Date:** 2026-08-04
**Scope:** `Tools/MonolithProxy` Windows build and publication

---

## 1. Root cause

Both native proxy build entry points compiled into the source tree, copied directly over `Binaries\monolith_proxy.exe`, left output-directory creation and publication unchecked, and printed success unconditionally. A failed or partial publication could therefore lie about the usable binary and risk changing an existing proxy.

## 2. Publication contract

| Phase | Contract |
|---|---|
| Compile | Write only to an invocation-owned private staging directory |
| Candidate | Copy beside the destination under a unique name and verify byte count |
| Replace | Rename the verified candidate over the target as the final publication step |
| Failure | Return non-zero, print no success, preserve the prior target before replacement, and remove owned candidates/staging |
| Entry points | `build_proxy.bat` owns the workflow; `build.bat` delegates without changing its exit code |

## 3. Verification

| Gate | Expected result | Result |
|---|---|---|
| Windows PowerShell 5.1 regression harness | Success through both entry points; injected compile and locked-target publication failures preserve sentinel bytes | Pass (`5.1.26100.8655`) |
| PowerShell 7 regression harness | The same build and failure contract holds in the current cross-shell runner | Pass (`7.5.5`) |
| Candidate/staging cleanup | No failure leaves an owned candidate or stage directory | Pass in both harness runs |
| Patch hygiene | `git diff --check` succeeds | Pass |

No Unreal module, asset, editor presentation, or gameplay surface changes. Screenshot and Discord upload verification are not applicable.
