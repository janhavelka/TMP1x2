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

auto status = sensor.begin(cfg);
if (status.ok()) {
  sensor.tick(applicationNowMs());
  status = sensor.startOneShot();
}
// In the application scheduler, without holding the bus lock between calls:
sensor.tick(applicationNowMs());
TMP1x2::Sample sample;
status = sensor.tryRead(sample);
if (status.ok()) {
  consumeTemperature(sample.celsius);
} else if (!status.is(TMP1x2::Err::MEASUREMENT_NOT_READY)) {
  handleError(status);
}
```

Read the [ownership and integration contract](docs/integration.md) before writing
callbacks. `end()` only deinitializes local state; `shutdown()` is the explicit
fallible hardware operation. All bus calls require external serialization.
`readSample()` returns the current temperature register; it cannot establish
freshness in continuous mode. One-shot completion is checked through hardware OS.
Changing extended format and entering shutdown from continuous mode require the
clock hook for TI's bounded settling sequence; ordinary continuous initialization
can run without it.

Supported addresses are 0x48–0x4B for the classic ADD0 mapping. TMP112D X2SON
address-select ordering codes at 0x40–0x43 are outside this release's supported
configuration. Model selection cannot establish physical identity. Extended
register range does not extend the part's specified operating range or accuracy.

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

Use this repository as a component through `EXTRA_COMPONENT_DIRS` or under your
application's `components/` directory. `library.json` is the version source;
`python scripts/generate_version.py sync` regenerates version metadata.

## References and validation

[TI datasheets, official source inventory and register notes](docs/reference/README.md)
are stored locally with source URLs and checksums. TI reference code retains its
own license; the independent driver and example code are MIT-licensed.

See [validation results](docs/validation.md) for the checks actually run and
[hardware validation](docs/hardware-validation.md) for the physical test procedure.
