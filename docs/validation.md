# Validation results

Local validation on 2026-09-22 (Windows, GCC 15.1 host compiler).

| Check | Result |
| --- | --- |
| CMake native core regression suite | Passed, 12 test groups |
| Exhaustive signed temperature decoding | Passed all 4,096 normal and 8,192 extended codes |
| Shared CLI behavioral tests | Passed help/ANSI formatting, color-off, cached diagnostics, invalid arguments, overflow rejection |
| Strict C++17 host warnings | Passed `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` |
| PlatformIO native tests | Passed all 12 test groups |
| Framework-free compile/link | Passed; no Arduino/Wire headers, including Xtensa macro-collision regression |
| Arduino ESP32-S3 and ESP32-S2 firmware | Compiled and linked with pioarduino 55.03.311 / Arduino 3.3.11 |
| Native IDF example against real SDK headers | Passed S3 and S2 with bundled IDF 5.5.5 SDK headers, Arduino include paths/defines removed |
| Standalone native ESP-IDF CMake/link | Not run locally: `idf.py`/standalone IDF unavailable |
| Release metadata and core boundary checks | Passed |
| PlatformIO release package | Created, checked and independently built with CMake; reference binaries and development tests excluded |
| Physical sensor / ALERT / address straps | Not run; no hardware results claimed |

Regression tests cover wire byte order and framing, signed conversion, threshold
rounding and overflow, invalid enum/address/timeouts without I2C, lifecycle,
probe versus tracked health, passive OFFLINE recovery, preserved failed outputs,
partial writes, dirty-state recovery, OS readiness, conversion deadlines, clock
wraparound, extended-format threshold preservation, all AL/POL combinations, and
each initialization transfer failing in turn. A dedicated device model reproduces
TI's premature EM marker; a failure after EM changes must retain the fresh-
conversion requirement through recovery.

SDK-header compilation is a narrower check than a complete native-IDF build.
The CI workflow adds native IDF 5.3.2, 5.5.1 and 6.0.1 builds for S2/S3, but these
jobs have not been run remotely. Host ASan/UBSan are configured in Linux CI and
are not claimed as locally run Windows sanitizer tests.

Reproduce native checks with the commands in the root README. For the narrower
SDK check, first generate the PlatformIO compilation database for the desired
target, then run `python tools/check_idf_sdk_compile.py`. The SDK checker writes
its response file under ignored `build/` and never links Arduino into IDF code.

Hardware procedure: [hardware-validation.md](hardware-validation.md).
