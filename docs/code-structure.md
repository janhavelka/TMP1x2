# Code organization

TMP1x2 follows the sibling libraries' public boundary: `include/TMP1x2/` holds
the driver, typed `Config`, `Status`, register constants and generated version.
`BusOperations.h` exposes explicit shared-bus commands separately from instance
initialization. Applications own the transport, clocks, GPIOs and serialization.

Most sibling libraries keep their implementation in one source file. TMP1x2
separates its larger implementation by responsibility to make protocol and
failure-state review easier, without changing the public API or object layout.

| Implementation | Responsibility |
| --- | --- |
| `src/TMP1x2.cpp` | Binding/lifecycle, physical register transfers, passive health, configuration evidence and cached settings |
| `src/TMP1x2Configuration.cpp` | Validation and encoding, the shared configuration state machine, typed setters, live readback and snapshots |
| `src/TMP1x2Measurement.cpp` | Signed temperature/threshold codecs, manual one-shots, measurement access and cached-sample freshness |
| `src/TMP1x2Operations.cpp` | Cooperative admission, transfer budgets, deadlines, cancellation and retained results |
| `src/BusOperations.cpp` | Explicit general-call reset and receive-only Alert Response |

Synchronous and cooperative configuration still use the same apply state machine.
All tracked register access goes through the same transport/health functions.
Temperature decoding has one implementation shared by diagnostic, manual and
owner reads. There are no platform-specific core implementations, hidden retries,
dynamic allocations or additional public helper classes.

Both firmware examples compile one shared CLI class:

| Example implementation | Responsibility |
| --- | --- |
| `examples/common/Tmp1x2Cli.cpp` | Setup, line parsing and command dispatch |
| `examples/common/Tmp1x2CliDiagnostics.cpp` | Cooperative operations, diagnostics, configuration tests and sampling runs |
| `examples/common/Tmp1x2CliOutput.cpp` | Help, ANSI styling, cached status and result rendering |
| Arduino / native IDF `main.cpp` | Framework transport adapters, board initialization and console servicing |

This separation follows the diagnostic/style boundaries in MB85RC, ADS1115 and
24Cxx examples while keeping Arduino and native IDF behavior in shared code.
CLI setup validates replacements before changing callbacks and rejects active
work or pending manual conversions. Successful idle reuse clears the old session's
input, results and statistics while retaining driver format uncertainty.

Standalone CMake and native IDF register the same explicit core source list.
PlatformIO includes `src/**` and `examples/common/**`. A custom build must compile
all five core source files, and all three shared CLI sources when using the example
processor. The SDK-header helper discovers these sources; the package check
requires them and links an isolated consumer across each core responsibility.
