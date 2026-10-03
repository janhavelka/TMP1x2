# Testing guide

Run commands from the repository root unless a command names another project.
Use a source checkout: release archives omit tests, runners and build scripts.
Record the commit with `git rev-parse HEAD` and local changes with
`git status --short` when saving results.

## Choose the checks

| Change | Required checks |
| --- | --- |
| Driver or shared CLI | Complete CMake/CTest suite and framework contract check |
| Public header, board configuration or framework adapter | Host checks plus Arduino and native ESP-IDF builds for ESP32-S2 and ESP32-S3 |
| HIL runner or CLI output | HIL parser tests, including the compiled CLI fixture; full CTest is the usual entry point |
| README, guides or public API comments | Doxygen generation and link/API check |
| Version, build files or package export | Version sync, full CTest, framework builds and isolated package consumer |
| Release intended for a PCB | All applicable checks above, followed by physical validation on each supported board/part/framework combination |

Host tests use a register model. Firmware builds prove compilation and linking.
Neither establishes physical timing, accuracy or electrical behavior. Keep
unperformed hardware checks marked `NOT_RUN`.

## Host suite

Install a C++17 compiler, CMake 3.16 or newer and Python 3.10 or newer. Ninja is
optional. No board SDK or third-party Python package is needed for host tests.
With GCC or Clang:

```sh
cmake -S . -B build-tests -DTMP1X2_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
python tools/check_contracts.py
```

On Windows, add `-G Ninja` when using an installed Ninja/GCC toolchain, or
`-G "MinGW Makefiles"` for MinGW. With Visual Studio, omit the GCC warning flags,
build with `--config Debug`, and pass `-C Debug` to CTest. Use a new build directory
when changing compilers or generators.

CTest runs core register tests, shared CLI tests, owner operations, model/address
variants, field helpers, shared-bus operations, framework-free headers, version
synchronization, IDF component naming, documentation checker regressions, and
the Python HIL runner tests. The HIL
tests launch the compiled CLI model automatically; its location is supplied by
CMake, including for multi-configuration generators. Test timeouts turn a stuck
test into a failure. Logs are in `build-tests/Testing/Temporary/LastTest.log`.

On Linux with GCC or Clang, use a separate sanitizer build:

```sh
cmake -S . -B build-sanitize -DTMP1X2_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

The Python HIL tests can also run directly:

```sh
python tools/test_hil_tmp1x2_runner.py
python tools/test_hil_tmp1x2_runner.py --cli-fixture build-tests/tmp1x2_cli_tests
```

Use `.exe` on Windows and the configuration subdirectory when required. The
first command skips tests that need the compiled fixture. A parser-only pass is
not the full runner regression result. `--dry-run` on the hardware runner lists
suites and side effects without opening a serial port or producing HIL evidence.

## Framework builds

On Windows use the existing VS Code-managed PlatformIO through this repository's
wrapper. Do not install another PlatformIO Core:

```powershell
.\scripts\pio.cmd test -e native
.\scripts\pio.cmd run -e native_core_no_arduino
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

PlatformIO `native` runs the core subset; the CMake suite above is required for
the CLI, job, helper, bus and Python tests. `native_core_no_arduino` compiles and
links a consumer without a framework. Both firmware commands must reach a full
application link and binary generation.

In an exported ESP-IDF environment, validate the native front end separately:

```sh
idf.py -C examples/esp_idf/basic set-target esp32s3 build
idf.py -C examples/esp_idf/basic set-target esp32s2 build
```

`set-target` reconfigures the example for the selected chip. Preserve any local
SDK configuration you need before switching. The standalone IDF component naming
test checks CMake registration only; it does not replace these builds.

## Version and release package

Edit `library.json` to change the version, then regenerate and check metadata:

```sh
python scripts/generate_version.py sync
python scripts/generate_version.py check
```

The generator updates the public version header, CMake, IDF metadata and Doxygen
version. Do not edit the generated header by hand. To verify a release archive
on Windows, create the output directory if needed and run:

```powershell
New-Item -ItemType Directory -Force build-package | Out-Null
.\scripts\pio.cmd pkg pack -o build-package/TMP1x2.tar.gz .
python tools/check_package.py build-package/TMP1x2.tar.gz --generator Ninja
```

Omit `--generator Ninja` to use the local CMake default. This checks required
files and forbidden generated/vendor artifacts, then extracts into a temporary
directory and builds, links and runs a renamed standalone consumer. Run it again
after changing export rules, source lists or public headers.

## Documentation

Install Doxygen, then run:

```sh
python tools/check_doxygen.py
```

The check fails on warnings, missing local links, missing public API or exposed
private API. It generates public HTML under `build/doxygen/html/`; open
`index.html`. The checker does not request external URLs. TI links may track
newer documents, so use the archived revisions and hashes for audit reproduction.

## CI and physical evidence

The GitHub Actions workflow runs Linux ASan/UBSan host tests and contract checks,
Doxygen checks, Arduino S2/S3 builds, a package consumer, and native ESP-IDF
5.3.2, 5.5.1 and 6.0.1 builds for S2/S3. Check the run for the exact pushed commit;
a successful run for an earlier commit does not validate later changes.

Follow the [serial runner guide](hil-runner.md) and
[hardware checklist](hardware-validation.md) when a board is available. Install
`pyserial` only in the Python environment used to open the serial port. Record
the firmware identity, board and BOM, selected suites, JSON report, transcript
and instrument traces. Run each framework separately. Treat failed cleanup as a
failed run and incomplete evidence as inconclusive; do not combine a partial
rerun with earlier output to claim a complete pass.

Keep observed results in [validation results](validation.md), with commands,
tool versions, commit or CI run link, and limitations. Commit and push a verified
logical block, then check its CI result before reporting it as complete.
