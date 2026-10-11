# Local library comparison

Re-audited on 2026-10-11 in the sibling `Projects/` directory, starting from
TMP1x2 `c8c93dd`. The inventory covers all 16 top-level sibling I2C libraries
found by manifest and public-header inspection. Application repositories,
archived copies, PlatformIO dependencies and worktrees are not separate
libraries. AT21CS11 uses a single-wire interface; ADS1261 and MAX31865 use SPI.
Those protocols are outside this comparison. No additional standalone I2C
library was found. The earlier [audit findings](audit-2026-09-26.md) remain
historical evidence.

## Detection, help and long-run follow-up

The follow-up starts from TMP1x2 `d957305`. All sixteen local repository states
were checked again. 24Cxx has advanced to `b6ddbc9`, adding a shared diagnostic
CLI and evidence-checked serial runner. TCA9548A remains at `07acf36` with
concurrent uncommitted CLI/build changes; that worktree was observed read-only.
The other fourteen revisions below are unchanged and clean. The earlier clean
worktree inventory remains a record of the preceding audit.

| Area | Strongest applicable examples inspected | TMP1x2 change |
| --- | --- | --- |
| Protocol-qualified detection | LDC1614 `examples/common/Ldc1614Cli.cpp`, INA228 and INA3221 `examples/01_basic_bringup_cli/main.cpp`; OPT4001 can use an identity register that TMP lacks | Cooperative address probe followed by CONFIG fixed-bit validation, with raw evidence, separate absence/incompatibility/errors and no automatic model/address selection |
| Discovery limits | SHT3x shared CLI and 24Cxx `examples/common/Eeprom24CxxCli.cpp` explicitly separate ACK from identity/geometry | Both broad scan and chip-address discovery report identity as unverified; TMP102/TMP112 cannot be distinguished by their four-register map |
| Navigable help | LDC1614 grouped help and targeted help; INA228/INA3221 aligned colored sections | Preserve the common colors and command column, add topic filtering and separate connection, owner, ALERT and register workflows |
| Sustained testing | LDC1614 duration/completion checks; LSM6DS3TR `examples/02_owner_soak/main.cpp` bounded progress; INA228 host soak aggregation | Host-controlled duration soak in finite cooperative batches, quiet sample output, bounded retained summaries, exact sample/transport/health checks and immediate failure reporting |
| USB and serial behavior | Peer PlatformIO settings plus the installed, pinned Arduino and ESP-IDF implementations | Explicit native USB input/output routes, bounded example output queue, loss counters, backend-specific DTR handling and reconnect/stall bench procedures |

SHT3x's low-output soak demonstrates useful compact reporting, but its blocking
loop is unsuitable for TMP's cooperative command service. INA228's configurable
row downsampling and INA3221's latency lists can still grow with duration. TMP
uses aggregate latency/count statistics and streamed transcripts with bounded
in-memory evidence. Long-run failures remain visible; the soak loop does not
recover automatically and resume as if the failed batch had passed.

SDK behavior matters as well as peer configuration: native IDF's previous
defaults selected UART input, and Arduino S2 TinyUSB requires DTR for output.
The common peer `monitor_dtr=0` setting does not establish S2 CDC readiness.
Console counters describe software buffering and transport acceptance; physical
host delivery and USB disconnect behavior still require the instrumented bench
checks in [hardware validation](hardware-validation.md).

## Python HIL runner follow-up

The runner audit starts from TMP1x2 `aff6c67` on 2026-10-11. The initial local
snapshot found 24Cxx at `b6ddbc9` and TCA9548A at `d715970`, both with uncommitted
changes. Both were inspected read-only. The other fourteen repositories were
clean at the revisions in the earlier inventory below. During the follow-up,
24Cxx advanced to clean `a318ee5`; this later observation does not replace the
initial snapshot. These are local checkout observations, not remote or flashed
firmware identities.

| Pattern | Applicable peer evidence | TMP1x2 follow-up |
| --- | --- | --- |
| Correlated serial responses | 24Cxx `tools/hil_runner.py`; INA228 `tools/run_i2c_hil.py`; INA3221 `tools/hil_cli_runner.py` | Unique nonce markers establish command boundaries; stale success text and unrelated prompts cannot certify the current command |
| Exactly one evidence record | MB85RC `tools/hil_runner.py`; RV3032-C7 `tools/hil_cli_runner.py`; SSD1315 `tools/run_ssd1315_hil.py` | Reject duplicate or contradictory terminal, sample and state records instead of accepting the first match |
| Deadline enforcement | SCD41 `tools/scd41_hil_runner.py`; BME280 `tools/run_i2c_hil.py` | Recheck the remaining budget before writing and after reading; retain late output without treating it as an on-time success |
| Measurement evidence across batches | SHT3x-main `tools/run_sht3x_hil.py`; SCD41 `tools/scd41_hil_runner.py` | Validate the batch minimum, mean and maximum against the temperature window and require acquisition timestamps to advance |
| Recoverable run evidence | 24Cxx `tools/hil_runner.py`; BME280 `tools/run_i2c_hil.py` | Atomically checkpoint the verified baseline before mutations; record host provenance at both start and end |
| Incomplete selected suites | LDC1614 `tools/ldc1614_hil_runner.py` | An unfinished or unrun selected suite prevents an overall PASS |
| Tests against the command processor | Peer parser tests and TMP1x2 `tools/test_hil_tmp1x2_runner.py` | Keep one native shared-CLI process alive through ordered suites and cleanup; use controlled serial chunks for stale, fragmented and delayed responses |

These additions extend the existing bounded transcript retention, quiet soak,
firmware identity checks and verified restoration. Reset or lost framing still
prevents automatic cleanup against an unchecked target. Parser tests and native
CLI fixtures establish software behavior only; physical sensor accuracy,
electrical faults, reset stimulus and USB disconnect behavior still need the
bench evidence described in [hardware validation](hardware-validation.md).

## Inventory

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
| 24Cxx | I2C EEPROM family | Shared CLI, strict numeric/input parsing, native-IDF packaging; page-write and write-cycle semantics differ |
| MCP45HVX1 | Digital potentiometer | Typed register API, bounded poll jobs |
| PCA9555 | GPIO expander | External ownership, separate transport/write-effect state |
| RV3032-C7 | RTC with temperature measurement | Common Status/Config layout and health; RTC persistence differs |
| SCD41 | CO2/temperature/humidity; delayed command/CRC frames | Owner-driven lifecycle; more specialized transfer abstraction |
| SSD1315 | OLED command/data streams | Cooperative transfer budgeting; no measurement/register analogue |
| TCA9548A | I2C switch control byte | Bus ownership discipline; different chip protocol |

Other peripheral protocols and application/board projects were outside the I2C
comparison. This inventory is background design research; none of these
repositories is a build or runtime dependency of TMP1x2.

## Inspected revisions and executable evidence

All sixteen sibling worktrees were clean on `main`, tracking their local
`origin/main` reference, at the revisions below. The audit read their manifests,
actual command dispatch/help, shared helpers, tests and hardware runners; it did
not fetch or modify the sibling repositories. Paths below are relative to the
named sibling repository. They identify inspected source, not build or hardware
results for TMP1x2.

| Library / revision | Actual command implementation | Testing and applicable evidence |
| --- | --- | --- |
| OPT4001 `082754b` | `examples/01_basic_bringup_cli/main.cpp`: full configuration, cached health, raw words, ALERT and cooperative jobs; `examples/common/HealthDiag.h` monitors cached health | `tools/hil_opt4001_runner.py`, `tools/check_cli_contract.py`; useful terminal layout and register/threshold workflows, but see the runner grammar limitation below |
| ADS1115 `1390991` | `examples/01_basic_bringup_cli/main.cpp`: configuration, thresholds, registers and `own` lifecycle/read/poll/cancel commands | `tools/run_i2c_hil.py`, `docs/ADS1115_HARDWARE_VALIDATION_PLAN.md`: firmware identity, bus-silent snapshots and absent-address procedures; Arduino owner commands are not mirrored by its native IDF CLI |
| SHT3x-main `93ec054` | `examples/common/Sht3xCli.cpp`: shared command processor, comprehensive help, manual job stepping and `xfer_assert` | `tools/run_sht3x_hil.py`: admission/progress transfer assertions and expected firmware commit; strong precedent for shared-framework behavior |
| BME280 `f66c2ee` | `examples/01_basic_bringup_cli/main.cpp`: complete staged settings, validation, apply, invalidation and job controls | `tools/run_i2c_hil.py`: validation silence, disconnect/invalidation, staged settings, cancellation/budget checks and firmware provenance |
| INA228 `4e10c70` | `examples/01_basic_bringup_cli/main.cpp`, `docs/CLI.md`: configuration, measurements, registers, diagnostics and framed test commands | `tools/run_i2c_hil.py`: expected firmware provenance and exact zero/one/multiple-transfer assertions |
| INA3221 `1ad5c69` | `examples/01_basic_bringup_cli/main.cpp`: `job auto`, `job step`, `hilrun`, selftest and cooperative stress | `tools/hil_cli_runner.py`: zero/one budgets, cancellation, retained results and terminal stress checks |
| LDC1614 `6ec9043` | `examples/common/Ldc1614Cli.cpp`, `examples/esp_idf/basic/main/Ldc1614IdfCli.cpp`: separate front ends with command metadata, detailed help, staged profiles, bus/state views, invalidation, jobs and bounded stress/soak | `tools/ldc1614_cli_contract.py`, `tools/ldc1614_hil_runner.py`, `test/test_cli.cpp`: output contracts, firmware identity, invalid-input state/transfer fences, correlated completion and mode/soak matrices |
| LSM6DS3TR `9edafd3` | `examples/01_basic_bringup_cli/main.cpp`, `examples/idf/basic/main/main.cpp`, `examples/common/ProfileCli.h`: staged profile, tokened operations, raw access and reconciliation | `tools/run_hil.py`: invalid-command transport/bus/profile invariants; `examples/02_owner_soak/main.cpp` and `tools/run_owner_soak.py` keep long soak output bounded |
| MB85RC `f888671` | `examples/01_basic_bringup_cli/main.cpp`, `examples/espidf_basic/main/main.cpp`, `examples/common/DiagnosticCore.h`: memory diagnostics, mode capabilities, staged transfers, scratch backup/restore and stress | `tools/_contract_data.py`, `tools/hil_runner.py`: framework command checks, malformed inputs, verified scratch restoration and bounded soak |
| 24Cxx `c74c1c6` | `examples/common/Eeprom24CxxCli.cpp`: shared CLI, strict parsing, bounded jobs, staged timeout/offline settings, counters and explicit scratch tests | `test/test_cli/test_cli.cpp`, `docs/hardware-validation.md`: executable parser/operation tests and physical procedures; no standalone serial HIL runner found |
| MCP45HVX1 `9573f5e` | `examples/01_basic_bringup_cli/main.cpp`, `tools/contract_common.py`: full wiper/TCON/register access and detailed command help, with safe/output-changing selftests | `tests/test_arduino_cli.cpp`, `tests/test_idf_console.cpp`, `tools/run_hil_mcp45hvx1.py`: parser execution, safe/mutating test groups and soak |
| PCA9555 `386fcd5` | `examples/01_basic_bringup_cli/main.cpp`: pin/port/mask/register controls, explicit mutation, restoration, selftest and stress | `tools/run_i2c_hil.py`, `docs/hardware_validation.md`: strict output and rejection checks; GPIO electrical behavior needs a fixture |
| RV3032-C7 `4dd51d1` | `examples/01_basic_bringup_cli/main.cpp`: calendar, temperature, alarms, flags, registers, RAM/EEPROM and backup provisioning | `tools/hil_cli_runner.py`, `docs/VERIFICATION.md`: functional/soak matrices, firmware metadata and complete health parsing; persistence/calendar tests are RTC-specific |
| SCD41 `70f8d80` | `examples/01_basic_bringup_cli/main.cpp`, `examples/common/DiagnosticWorkflow.h`: lifecycle, measurements, all configuration/calibration commands, raw protocol and mode-valid diagnostic jobs | `tools/scd41_hil_runner.py`, `docs/validation/hardware-hil.md`: operation/result correlation and explicit slow or nonvolatile procedures |
| SSD1315 `9c4d2a6` | `examples/01_basic_bringup_cli/main.cpp`, `examples/espidf_basic/main/main.cpp`: controller/graphics access, partial flush, selftest, stress and compact `soakstep` | `tools/run_ssd1315_hil.py`, `docs/SSD1315_HIL_RUNBOOK.md`: firmware metadata and functional/retention/soak matrices; optical checks remain separate |
| TCA9548A `07acf36` | `examples/01_basic_bringup_cli/main.cpp`, `examples/espidf_basic/main/main.cpp`: route masks, cached evidence, invalidation, lifecycle and diagnostics | `tools/tca9548a_hil.py`, `docs/VALIDATION_STATUS.md`: exact mask readback, verified restoration, all-mask and branch-isolation tests; host checkout identity is explicitly not flashed firmware identity |

The six closest sensor repositories' CLI contract checkers passed during the
read-only audit. This does not prove complete parity. In ADS1115, the checker
inspects the Arduino CLI while native IDF lacks the Arduino `own` command
routes. In OPT4001, the serial runner contains older conversion-time/range/mode
command forms that differ from the current native IDF dispatcher. Actual
dispatch and executable output therefore take precedence over a passing text
checker or an old runner command list. TMP1x2 keeps one shared command processor
and runs its serial parser against that processor's native fixture.

## Transferable audit findings

The common requirement is access to every applicable chip feature, observable
operation state, strict inputs and repeatable tests. A command with the same
name can have different side effects across chips: a peer's `selftest` may read
registers, drive outputs, consume EEPROM endurance or run an internal chip test.
TMP1x2 documents its read-only checks separately from the full configuration
exercise and shared-bus maintenance.

- **Owner scheduling:** SHT3x, INA228 and INA3221 expose transfer-budget and
  cancellation evidence. Manual stepping and exact counter assertions make the
  TMP owner's zero/one-transfer contracts reviewable from the CLI.
- **Configuration trust:** BME280, LDC1614 and TCA9548A expose explicit local
  invalidation. Cached trust must remain separate from successful transport;
  raw/out-of-band writes need verified recovery before trusted sampling.
- **Strict command admission:** LSM6DS3TR and LDC1614 hardware runners compare
  state and transfer counters around malformed-input matrices. Host parser
  tests and serial tests cover different boundaries; neither should accept
  rejection text as proof that no I2C occurred.
- **Reproducible reports:** distinguish host source revision from flashed
  firmware. At the inspected TMP baseline, Arduino already printed Git metadata
  through `TMP1X2_VERSION_FULL`; native IDF left those macros unknown. The native
  example now refreshes an example-only identity header at every build, including
  incremental edits. Native CMake and the PlatformIO pre-build script use the
  same generator; only CLI output includes the result. Source archives retain
  unknown Git identity even when
  extracted inside another repository. The core has no Git/build dependency.
- **Diagnostic context:** cached pins, frequency, transport backend and timeout
  make a transcript useful for reproducing a board setup. Adapter counters
  include probes/raw traffic and must remain distinct from tracked health.
- **Stress and restoration:** finite counts, bounded durations, retained
  results, cancellation and verified cleanup are useful across libraries.
  Successful serial traffic is not thermal accuracy, exact ALERT timing,
  electrical fault tolerance or validation of another board/framework.

The TMP chip provides no identity register, CRC, FIFO, EEPROM, calibration
offset, channel mux or graphics interface. Those peer commands are excluded
deliberately. TMP general-call reset and SMBus Alert Response retain their own
explicit bus-wide/acknowledgement semantics. Host-controlled soak and physical
fault/thermal fixtures complement the existing configuration, one-shot, stress
and restoration tests; there is no universal sibling test that certifies a chip.

## Chosen conventions

OPT4001 supplies the repository shape and terminal presentation. ADS1115 supplies the most directly comparable register-transport contract. SHT3x demonstrates how one framework-neutral command processor can give identical Arduino/native IDF CLI behavior without compiling Arduino facades into IDF. Sensor protocol facts must come from TI rather than any sibling chip driver.

- Public headers under `include/TMP1x2/`: `TMP1x2.h`, `Config.h`, `Status.h`, `CommandTable.h`, generated `Version.h`; implementation under `src/`.
- Core and shared CLI implementations are grouped by responsibility; see the
  [code organization guide](code-structure.md). Public headers retain the common
  sibling layout. Most peers use one core source file; the internal split is a
  maintainability choice, not a requirement for API compatibility.
- C++17 core without Arduino, ESP-IDF, FreeRTOS, logging, platform timing, allocation, bus handles or pins. Example adapters own those resources.
- `Status { Err code; int32_t detail; const char* msg; }`, static message strings, `ok()`, `is()`, `inProgress()`, explicit bool, `Ok()` and `Error()` factories. Typed `enum class ... : uint8_t`, uppercase enum values/constants, camelCase methods and fields, `_camelCase` members.
- `I2cWriteFn = Status (*)(uint8_t, const uint8_t*, size_t, uint32_t timeoutMs, void*)`; `I2cWriteReadFn` adds TX/RX buffers and lengths and performs one atomic repeated-start transfer. Caller owns locking, bus lifecycle, pins, clock frequency, timeout enforcement, scheduling and recovery. Preserve meaningful transport errors; do not invent NACK phase information.
- Bus-silent binding, admission, cancellation and end; one owner operation per instance with caller-driven polls. Initialization, configuration, recovery, shutdown and read jobs have transfer budgets, deadlines and retained results. Synchronous conveniences share the same configuration state machine. See [owner operations](owner-operations.md) and [integration](integration.md). No automatic general-call reset or hidden retry.
- Four health states: `UNINIT`, `READY`, `DEGRADED`, `OFFLINE`; saturating success/failure counters, consecutive failures, last success/error times and last error. Configuration trust is separate from transport health. Diagnostic probes do not alter tracked driver health. Bus-adapter counters include diagnostic traffic and therefore differ from driver counters.
- TMP1x2 adopts the OPT4001 help style: cyan `=== TMP1x2 CLI Help ===`, green group headings; cyan left-aligned command column width 32; plain descriptions; prompt `> `. Topic filtering follows the LDC1614 pattern, with common, connection, data, owner job, configuration, ALERT, register and diagnostic groups. ANSI reset `ESC[0m`, red 31, green 32, yellow 33, cyan 36 and gray 90. `color [0|1|off|on]` disables styling. Health is green for READY, yellow for degraded/uninitialized, red for offline/errors.
- Common CLI aliases include `help/?`, `version/ver`, `init/begin`, `drv/health`, `cfg/settings`, `reg/rreg`, plus scan, probe, recover, end, cached sample, finite watch/stress, stop and raw register diagnostics. Job/result/cancel commands expose cooperative progress; quiet mode, run statistics, counter reset and optional ALERT GPIO mirror peer diagnostics. Chip-specific commands follow actual TMP capabilities; no fictitious chip ID or CRC command.
- Native IDF examples use `app_main`, `driver/i2c_master.h`, `esp_timer` and task timing; no Arduino compatibility layer. Both adapters exercise the same command processor.
- Sibling build conventions: `library.json` as version source, generated version header and component manifest, C++17 CMake component, PlatformIO S2/S3 Arduino environments and native tests, Windows `scripts/pio.cmd`, explicit framework-free compilation, CLI-contract checks. Hardware validation is a separate claim from builds and simulated transport tests.

These sibling libraries differ in health vocabulary, lifecycle semantics and
cooperative method names. LDC1614 and LSM6DS3TR expose transport statistics
alongside separate operation/configuration state. Status enum ordinals are not
a shared ABI, and device-specific errors vary. Matching every historical API
byte-for-byte is neither possible nor appropriate; TMP1x2 follows the common
conventions while keeping its sensor protocol explicit.

The [feature coverage matrix](feature-coverage.md) also maps canonical initializer,
settings/sample and register-word names, field helpers, full configuration tests
and the chip's explicit shared-bus commands. `init` and bound-profile `begin`
retain verified sensor initialization; `getSettings`, `getLastSample`,
`readRegister16`/`writeRegister16` and `invalidateDeviceState` follow applicable
sensor-library names without changing the older API's behavior.
