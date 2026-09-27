# Validation results

Local validation on 2026-09-26 after the [cross-library audit](audit-2026-09-26.md).
Windows host, GCC 15.1.0, Python 3.12.10, managed PlatformIO Core 6.1.19.

| Check | Result |
| --- | --- |
| CMake/CTest | All nine entries passed: core, CLI, owner operations, model variants, field helpers, shared-bus operations, framework-free headers, version synchronization and IDF component naming |
| Exhaustive signed temperature decoding | Passed all 4,096 normal and 8,192 extended codes |
| Shared CLI behavioral tests | Passed help/ANSI, parsing, cooperative operations/cancellation, raw-health separation, GPIO/model switching, quiet statistics/counter reset, mixed stress in both modes, target binding, transient failures and watch stop/timeout/resumption |
| Strict C++17 host warnings | Passed `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` |
| PlatformIO native tests | Passed all 18 test groups |
| Cooperative owner API | Passed 14 independent groups covering admission, budgets, deadlines/wrap, staging, failure matrices, cancellation, retained results and passive health |
| Model/address capabilities | Passed all 128 seven-bit addresses across three models, valid callback targets, rejected GPIO capability and preserved failed outputs |
| Field helpers | Ten groups passed: validation/encoding, unit/count conversions, aliases, acquisition provenance/age, one-shot provenance, health/invalidation, live snapshots, partial-failure evidence, interrupted threshold observations and direct prior-format adoption |
| Shared-bus operations | Exact frames and bounded one-callback behavior, silent invalid arguments, preserved failed outputs, all 256 ARA bytes and all supported model/address/POL/status cause combinations passed |
| Comprehensive CLI workflows | Cooperative scan/self-check, all 18 configuration cases plus restore, first/second cancellation, restoration failures, extended-only baseline limits, pending-shot rejection, reset scope, ARA trust and platform-only timestamp regressions passed |
| CLI setup reuse | Active owner/watch/full-test/manual-conversion replacement rejected without I2C; invalid idle replacement preserves callbacks; successful reuse clears stale input/results/statistics and retains format evidence |
| Format-transition failure matrix | Passed all 44 cases: 11 callbacks in both directions, failing writes accepted/rejected by hardware |
| Framework-free compile/link | Passed; no Arduino/Wire headers, including Xtensa INTERRUPT and Arduino LOW/HIGH macro-collision regressions |
| Arduino ESP32-S3 and ESP32-S2 firmware | Compiled and linked with pioarduino 55.03.311 / Arduino 3.3.11 |
| Native ESP-IDF ESP32-S3 and ESP32-S2 firmware | Full application/component compile/link, bootloader and binary generation using IDF 5.5.5 through managed PlatformIO |
| Release metadata and core boundary checks | Passed, including CMake version synchronization |
| PlatformIO release package | Required payload and exclusions inspected; isolated renamed CMake consumer compiled, linked and ran |
| TI reference archive | All recorded SHA-256 hashes and artifact coverage checked; archive unchanged |
| Physical sensor / ALERT / address straps | Not run; no hardware results claimed |

Regression tests cover wire byte order and framing, signed conversion, threshold
rounding and overflow, invalid enum/address/timeouts without I2C, lifecycle,
probe versus tracked health, passive OFFLINE recovery, preserved failed outputs,
partial writes, dirty-state recovery, OS readiness, conversion deadlines, clock
wraparound, extended-format threshold preservation, all AL/POL combinations, and
each initialization transfer failing in turn. A dedicated device model reproduces
TI's premature EM marker; a failure after EM changes must retain the fresh-
conversion requirement through recovery.

New format-provenance tests first reproduced the erroneous 25 C to 50 C reading,
then checked managed CONFIG observers, mismatched TEMP markers (including invalid
reserved bits), interrupted threshold/recovery observations, absent clocks,
adoption/restoration of EM and evidence retained through end/bind/begin. Unrelated
same-format configuration mismatches still recover without a clock.

The owner-operation suite covers clock-hook and caller-time scheduling, callback
timeout grants, deadline-capped next-poll hints, late sample rejection, zero
transfer budget, immediate completion after the final callback, accepted/rejected
partial writes in both EM directions, staged-profile cancellation, exact result
identity, pending-result exclusion, and uncertainty retained after abandoning a
manual one-shot. A stalled caller clock returns promptly without transfers during
waits; the caller still owns advancing monotonic time.

The helper suite also round-trips every representable normal/extended count
through Celsius encoding, distinguishes unknown acquisition time from a valid
timestamp of zero, and checks stale/dirty cache rejection without modifying output.
Snapshot tests fail each of five reads, retain early CONFIG/TEMP/threshold evidence
before later failures, and distinguish dynamic OS/AL changes from persistent
configuration changes. Resetting statistics preserves OFFLINE and last-fault
evidence. Shared-bus tests keep both ARA status-bit values and verify published
TMP102/TMP112 cause differences without assigning an identity to generic responders.

The final review adds 16 interrupted-threshold cases and 12 direct format-adoption
cases. It reproduces marker-only external EM changes followed immediately by a
synchronous setter or rebinding, without a prior diagnostic observation. Both
format directions require guarded conversion before trusted decoding; absent
clock hooks preserve uncertainty for cooperative recovery. The structural split
was also checked for all 87 existing method definitions with unchanged signatures.

The native IDF results are full application/component builds using `framework =
espidf`, not SDK-header compilation or an Arduino compatibility layer. Both target
configurations were checked for 4 MB flash and package version 1.0.0. The separate
`idf.py` front end was not run locally. The earlier SDK-header-only validation is
superseded by these local links. The IDF component naming regression is a CMake
configure test and does not itself establish a native IDF link.

CI is configured for native IDF 5.3.2, 5.5.1 and 6.0.1 on S2/S3, plus Linux
ASan/UBSan and package checks. Remote CI and Linux sanitizer execution were not
run in this session.

Two initial-audit runs encountered transient missing-module errors in managed SCons
(`win32` and `FortranCommon`). Rerunning the affected environment succeeded;
no second PlatformIO Core or repository workaround was installed. The final S2
IDF success is recorded separately in `build-audit/idf-final-s2.log`. The follow-up
owner/CLI changes were rebuilt for all four firmware environments successfully;
their final logs are `build-parity/arduino.log` and `build-parity/idf.log`.
The subsequent complete-feature/field-helper pass also compiled and linked all
four firmware environments, including the new receive-only/general-call adapter
paths and `BusOperations` component source; final logs are under `build-field/`.
The final datasheet/structure review rebuilt the split core and CLI for all four
firmware environments successfully. Strict CTest passed all nine entries, the
helper suite passed ten groups, native PlatformIO passed 18 groups, and the
framework-free target linked. Final firmware logs are under `build-recheck/`.

Reproduce the checks from the repository root:

```powershell
cmake -S . -B build-recheck -G Ninja -DTMP1X2_BUILD_TESTS=ON "-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
cmake --build build-recheck --parallel
ctest --test-dir build-recheck --output-on-failure
python tools/check_contracts.py
.\scripts\pio.cmd test -e native
.\scripts\pio.cmd run -e native_core_no_arduino -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
.\scripts\pio.cmd pkg pack -o build-recheck/TMP1x2-recheck.tar.gz .
python tools/check_package.py build-recheck/TMP1x2-recheck.tar.gz --generator Ninja
```

Latest firmware logs and release archive are under ignored `build-recheck/`;
earlier logs remain under `build-audit/`, `build-parity/` and `build-field/`.
No board was flashed. The [hardware procedure](hardware-validation.md) remains
the next step for electrical, conversion-timing and ALERT behavior evidence.
