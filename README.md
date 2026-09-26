# TMP1x2

Framework-neutral C++17 driver for TI **TMP102 and TMP112** temperature sensors.
The public API, transport callbacks, four-state health model, repository layout
and colored diagnostic CLI follow the neighboring OPT4001 library, with ADS1115
as the four-register protocol reference. See the full
[library comparison](docs/library-comparison.md).

- Signed 12-bit normal and 13-bit extended temperatures, 0.0625 C per count.
- Continuous rates of 0.25, 1, 4 and 8 Hz; shutdown and polled one-shot conversion.
- Low/high thresholds, comparator/interrupt ALERT, polarity and fault queue.
- Structured `Status`, typed enums, raw diagnostics, verified configuration,
  dirty-state detection and explicit recovery.
- Application-owned I2C callbacks; no Arduino/ESP-IDF dependency in the core.
- Cooperative initialization/configuration/recovery/shutdown/read operations with
  transfer budgets, cancellation, deadlines and exactly-once results.
- Explicit TMP112D X2SON address-select support at 0x40..0x43, with model-specific
  address and physical ALERT capability checks.
- Arduino ESP32-S2/S3 and native ESP-IDF examples sharing one diagnostic CLI.

## Integration

```cpp
#include <TMP1x2/TMP1x2.h>

TMP1x2::TMP1x2 sensor;
TMP1x2::Config cfg;
// Implement these bounded callbacks in your application's bus owner:
cfg.i2cWrite = applicationWrite;
cfg.i2cWriteRead = applicationWriteRead;
cfg.i2cUser = &applicationBus;
cfg.nowMs = applicationClock;    // uint32_t applicationClock(void*)
cfg.cooperativeYield = applicationYield; // void applicationYield(void*)
cfg.model = TMP1x2::Model::TMP112;  // explicit BOM selection; no chip ID exists
cfg.i2cAddress = 0x48;
cfg.mode = TMP1x2::Mode::SHUTDOWN;

TMP1x2::OperationToken token = 0; // retain with the driver between scheduler turns
auto status = sensor.bind(cfg);  // no I2C
if (status.ok()) status = sensor.startInitialize(applicationNowMs(), 500, token);

// In the owning scheduler, without holding a bus lock between calls:
if (sensor.operationActive()) {
  const auto progress = sensor.poll(applicationNowMs(), 1); // at most one transfer
  if (progress.done) {
    TMP1x2::OperationResult result;
    if (sensor.takeResult(token, result).ok()) {
      if (!result.status.ok()) handleError(result.status);
      else if (result.kind == TMP1x2::OperationKind::INITIALIZE)
        status = sensor.startRead(applicationNowMs(), 500, token);
      else if (result.hasSample) consumeTemperature(result.sample.celsius);
    }
  }
}
```

Read the [ownership and integration contract](docs/integration.md) before writing
callbacks and the [owner-operation contract](docs/owner-operations.md) before
integrating a scheduler. `end()` deinitializes local state and retains a cancelled
job's result; `shutdown()`/`startShutdown()` explicitly change hardware mode.
All bus calls require external serialization. Synchronous `begin()`, typed
setters, `recover()` and measurement helpers remain available.
`readSample()` returns the current temperature register; it cannot establish
freshness in continuous mode. One-shot completion is checked through hardware OS.
Changing extended format and entering shutdown from continuous mode use TI's
settling sequence. Owner jobs can use caller time; synchronous helpers need the
clock hook for those waits.

Supported addresses are 0x48–0x4B for classic TMP102/TMP112 and fixed-address
TMP112 ALERT variants. Select `Model::TMP112D_ADDRESS_SELECT` for the X2SON ADD0
variant at 0x40–0x43, which has no physical ALERT pin. Model selection cannot
establish physical identity. Extended register range does not extend the part's
specified operating range or accuracy.

## Build and test

Native, with no framework or third-party test dependency:

```sh
cmake -S . -B build -DTMP1X2_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
python tools/check_contracts.py
```

On Windows with MinGW, add `-G "MinGW Makefiles"` to the configure command.
The existing VS Code-managed PlatformIO installation is used through:

```powershell
.\scripts\pio.cmd test -e native
.\scripts\pio.cmd run -e native_core_no_arduino
.\scripts\pio.cmd run -e esp32s3dev -e esp32s2dev
```

Configure example pins in `examples/common/BoardConfig.h` before connecting
hardware. The firmware examples are diagnostic bring-up applications; deployment
bus locking and retry policy belong to your application.

Native ESP-IDF (5.3 or newer, `driver/i2c_master.h` API):

```sh
idf.py -C examples/esp_idf/basic set-target esp32s3
idf.py -C examples/esp_idf/basic build
```

The same native IDF example also builds with the existing managed PlatformIO:

```powershell
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

Use this repository as a component through `EXTRA_COMPONENT_DIRS` or under your
application's `components/` directory. `library.json` is the version source;
`python scripts/generate_version.py sync` regenerates version metadata.

## References and validation

[TI datasheets, official source inventory and register notes](docs/reference/README.md)
are stored locally with source URLs and checksums. TI reference code retains its
own license; the independent driver and example code are MIT-licensed.

See [validation results](docs/validation.md) for the checks actually run and
[hardware validation](docs/hardware-validation.md) for the physical test procedure.
The [2026-09-26 audit](docs/audit-2026-09-26.md) records sibling-library parity,
confirmed defects, fixes and remaining design differences.
