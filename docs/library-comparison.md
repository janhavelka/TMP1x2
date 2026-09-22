# Local library comparison

Inspected on 2026-09-22 in the sibling `Projects/` directory. The inventory covers every top-level standalone I2C library found by manifest and public-header inspection. Application repositories, archived copies, PlatformIO dependencies and worktrees are not counted as separate libraries.

| Library | Device / protocol | Relevance to TMP102/TMP112 |
| --- | --- | --- |
| OPT4001 | TI light sensor; 16-bit pointer registers, thresholds, one-shot/continuous, ALERT | Closest overall example/API/CLI template |
| ADS1115 | TI ADC; four 16-bit registers at 0x00–0x03, addresses 0x48–0x4B | Closest register-layout and addressing analogue; owner-safe transport contracts |
| SHT3x-main | Temperature/humidity; command words and CRC | Closest measurement domain; shared platform-neutral CLI implementation |
| BME280 | Temperature/humidity/pressure; byte registers and compensation | Health/configuration-trust separation, IDF integration |
| INA228 | TI power monitor; mixed-width registers | Cooperative jobs, passive health, explicit transfer limits |
| INA3221 | TI triple power monitor; 16-bit registers | Register transport, sample/configuration provenance |
| LDC1614 | TI inductance converter; 16-bit registers | Typed chip variants, externally driven cooperative operations |
| LSM6DS3TR | IMU; byte registers and FIFO | Framework-neutral callbacks, operation/configuration state |
| MB85RC | I2C FRAM family | External ownership and partial-write reporting; memory semantics differ |
| MCP45HVX1 | Digital potentiometer | Typed register API, bounded poll jobs |
| PCA9555 | GPIO expander | External ownership, separate transport/write-effect state |
| RV3032-C7 | RTC with temperature measurement | Common Status/Config layout and health; RTC persistence differs |
| SCD41 | CO2/temperature/humidity; delayed command/CRC frames | Owner-driven lifecycle; more specialized transfer abstraction |
| SSD1315 | OLED command/data streams | Cooperative transfer budgeting; no measurement/register analogue |
| TCA9548A | I2C switch control byte | Bus ownership discipline; different chip protocol |

Other top-level standalone libraries were classified as non-I2C: ADS1261_ESP32 and MAX31865 (SPI); AT21CS11 (single wire); EE871-E2 (E2); AsyncSD (SD); SHZK-PT, VibWire-108 and VTN4xx (serial codecs); SIM7080G-Core (UART modem); StatusLED (LED/RMT); SystemChrono (time). LGClimateLink is a separate climate-control protocol project. ESP32_Interfaces is an interface application with a RuntimeProbe helper, and NextionNX3224T028_UART is a display application. Remaining top-level entries are applications, board/design projects, this repository, or TunnelMonitor archive/worktree copies.

## Chosen conventions

OPT4001 supplies the repository shape and terminal presentation. ADS1115 supplies the most directly comparable register-transport contract. SHT3x demonstrates how one framework-neutral command processor can give identical Arduino/native IDF CLI behavior without compiling Arduino facades into IDF. Sensor protocol facts must come from TI rather than any sibling chip driver.

- Public headers under `include/TMP1x2/`: `TMP1x2.h`, `Config.h`, `Status.h`, `CommandTable.h`, generated `Version.h`; implementation under `src/`.
- C++17 core without Arduino, ESP-IDF, FreeRTOS, logging, platform timing, allocation, bus handles or pins. Example adapters own those resources.
- `Status { Err code; int32_t detail; const char* msg; }`, static message strings, `ok()`, `is()`, `inProgress()`, explicit bool, `Ok()` and `Error()` factories. Typed `enum class ... : uint8_t`, uppercase enum values/constants, camelCase methods and fields, `_camelCase` members.
- `I2cWriteFn = Status (*)(uint8_t, const uint8_t*, size_t, uint32_t timeoutMs, void*)`; `I2cWriteReadFn` adds TX/RX buffers and lengths and performs one atomic repeated-start transfer. Caller owns locking, bus lifecycle, pins, clock frequency, timeout enforcement, scheduling and recovery. Preserve meaningful transport errors; do not invent NACK phase information.
- Bus-silent binding and end/cancellation; one cooperative operation per instance, caller-driven bounded polls and explicit waits. Synchronous convenience helpers must document their finite transfer bounds. No automatic general-call reset or hidden retry.
- Four health states: `UNINIT`, `READY`, `DEGRADED`, `OFFLINE`; saturating success/failure counters, consecutive failures, last success/error times and last error. Configuration trust is separate from transport health. Diagnostic probes do not alter tracked driver health. Bus-adapter counters include diagnostic traffic and therefore differ from driver counters.
- OPT4001 help uses cyan `=== TMP1x2 CLI Help ===`, green `[Common]`, `[Data]`, `[Configuration]`, `[Registers]`, `[Diagnostics]` sections; cyan left-aligned command column width 32; plain descriptions; prompt `> `. ANSI reset `ESC[0m`, red 31, green 32, yellow 33, blue 34, cyan 36, gray 90. `color [0|1|off|on]` disables all ANSI styling. Severity tags alone are colored. Health is green for READY, yellow for degraded/uninitialized, red for offline/errors.
- Common CLI aliases include `help/?`, `version/ver`, `init/begin`, `drv/health`, `cfg/settings`, `reg/rreg`, plus scan, probe, recover, end, cached sample, finite watch/stress, stop and raw register diagnostics. Chip-specific commands follow actual TMP capabilities; no fictitious chip ID or CRC command.
- Native IDF examples use `app_main`, `driver/i2c_master.h`, `esp_timer` and task timing; no Arduino compatibility layer. Both adapters exercise the same command processor.
- Sibling build conventions: `library.json` as version source, generated version header and component manifest, C++17 CMake component, PlatformIO S2/S3 Arduino environments and native tests, Windows `scripts/pio.cmd`, explicit framework-free compilation, CLI-contract checks. Hardware validation is a separate claim from builds and simulated transport tests.

These sibling libraries deliberately differ in legacy health gating and cooperative method names. Matching every historical API byte-for-byte is neither possible nor appropriate; TMP1x2 follows the common conventions while keeping its sensor-specific protocol explicit.
