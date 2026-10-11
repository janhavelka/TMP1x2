# Diagnostic examples

The [serial HIL runner](../docs/hil-runner.md) drives either example and saves
JSON results and a transcript. Build and flash the chosen example separately,
configure its model/address, and close the serial monitor before running it.

Both examples run the same framework-neutral command processor in `common/Tmp1x2Cli*.cpp`, split into command dispatch, diagnostics and output. Arduino and native ESP-IDF therefore have the same commands, aliases, help layout, ANSI colors, finite sampling workflows and parsing behavior. These are bring-up diagnostics, not a multitask bus-manager implementation.

`common/BoardConfig.h` selects SDA 8, SCL 9, 400 kHz and a 50 ms transaction timeout. Override `TMP1X2_I2C_SDA`/`TMP1X2_I2C_SCL` or edit this example-only file for your hardware. Optional `TMP1X2_ALERT_PIN` defaults to -1 (disabled); configure an input with appropriate external pull-up for physical ALERT diagnostics. Fit appropriate external I2C pull-ups. Platform pin setup and handles stay in the examples.

Set `TMP1X2_MODEL` (`0` TMP102, `1` TMP112, `2` TMP112D address-select) and
`TMP1X2_I2C_ADDRESS` before flashing: startup initializes that target before the
CLI accepts commands. The default address is `0x48` for models 0/1 and `0x40`
for model 2. For a TMP112 strapped to `0x49`, add `-DTMP1X2_MODEL=1` and
`-DTMP1X2_I2C_ADDRESS=0x49` to the selected environment's `build_flags`, or edit
the shared header. Both Arduino and native IDF use these settings. Invalid
model/address or unsupported ALERT combinations fail compilation.

`TMP1X2_I2C_FREQUENCY_HZ` selects ordinary I2C up to 400000 Hz (default 400000);
these example adapters do not issue the high-speed master code.
`TMP1X2_I2C_TIMEOUT_MS` defaults to 50 and accepts 1..65535 ms to fit both
adapters. The core's callback timeout contract remains independent of this
example-specific limit. `TMP1X2_SERIAL_BAUD` defaults to 115200 for Arduino;
native ESP-IDF's console baud and routing are selected in its SDK configuration.
Match the monitor or HIL runner's baud to the firmware console.

`bus` reports the configured pins, clock and transfer timeout
without I2C. `version` includes a separate `Firmware: git=... tree=... built=...`
line. Arduino and native IDF builds identify the source checkout; an exported
archive without Git reports unknown source identity. A host checkout revision
does not identify an already flashed binary. Use the runner's expected-firmware,
framework and target checks when recording reproducible results.

## Arduino / PlatformIO

Build `esp32s3dev` or `esp32s2dev` using the root `platformio.ini`. Open the 115200 baud monitor. The application creates and owns `Wire`; the driver receives only write and repeated-start read callbacks. Only `setup`/`loop` access the driver or I2C. Wire short-read errors retain generic I2C status because the returned byte count cannot prove which phase failed.

## Native ESP-IDF

From `esp_idf/basic`, with an activated ESP-IDF 5.3 or newer environment:

```text
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Use `esp32s2` for S2. Fresh SDK configurations select USB Serial/JTAG on S3 and
ROM USB CDC on S2, with a 1000 Hz FreeRTOS tick. Explicit selections in `sdkconfig`
override defaults; SDK-generated default values can follow updated defaults on
regeneration. Inspect the generated console and tick settings, or select them
with `idf.py menuconfig`. Preserve a copy of custom SDK settings before changing
build configurations. UART consoles remain supported; use `console` to check the
running backend.

The existing VS Code-managed PlatformIO can also build the same native IDF
application from the repository root:

```powershell
.\scripts\pio.cmd run --project-dir examples/esp_idf/basic -e esp32s3 -e esp32s2
```

This uses ESP-IDF directly, with the same pinned platform as the Arduino examples.
Both board configurations use a 4 MB flash layout. The library component name is
derived from the checkout directory, so renamed release archives also work.

The application creates one `i2c_master` bus, eight fixed sensor handles for
0x40–0x43 and 0x48–0x4B, and two dedicated handles for explicit general-call/ARA
commands at 0x00 and 0x0C. Creating handles does not probe or reset a device. An
input task queues characters with backpressure; only `app_main` calls the command
processor, driver and bus. There is no Arduino API or compatibility facade in this
example. The root component has no framework dependencies; the example depends
on IDF I2C, timer, GPIO, console drivers and task facilities.

## USB and long runs

| Example | Default console | Monitor DTR / RTS |
| --- | --- | --- |
| Arduino S3 | USB Serial/JTAG | 0 / 0 |
| Arduino S2 | TinyUSB CDC | 1 / 0 |
| Native IDF S3 | USB Serial/JTAG | 0 / 0 |
| Native IDF S2 | ROM USB CDC | 1 / 0 |

Use the board's native USB connector for these defaults. Arduino S2 needs DTR
asserted to transmit; the HIL runner therefore needs `--dtr 1 --rts 0` there.
Choose line settings for the actual console when overriding it with a UART
bridge. Avoid changing baud to 1200 or toggling reset/boot lines during a run.

Both adapters queue CLI output in a fixed 16 KiB buffer. Formatting does not wait
for a host. Each loop services at most 64 input characters, advances cooperative
work, offers at most 256 output bytes to a ready backend and yields. The S2 ROM
CDC path offers at most 64 bytes per write. Arduino USB uses a zero TX timeout;
native IDF uses nonblocking backend writes. Input backpressure and console queues
belong to the example adapters; the core remains framework-free.

`console` reports the actual backend, queued bytes, accepted bytes, dropped bytes
and oversized messages. Accepted bytes do not prove host receipt. A stalled host
can fill the buffer; complete formatted chunks are then dropped and an
`[E] CONSOLE_OUTPUT_LOST` marker is queued when space returns. Counters saturate
instead of wrapping. Use `verbose 0` for long stress runs, and treat new loss as
failed evidence. Watch elapsed time uses a 64-bit accumulator, provided the loop
continues servicing its 32-bit clock at least once per clock period.

The [runner](../docs/hil-runner.md) provides duration soaks and checks console
loss. The [hardware procedure](../docs/hardware-validation.md) covers host stalls,
data-cable disconnects and independent I2C observation. Builds and mocked writes
do not establish physical USB behavior.

## Typical session

`help` shows all command groups with the same layout and colors in both
frameworks. `help connection`, `help data`, `help job`, `help config`,
`help alert`, `help registers` and `help diagnostics` show one group;
`help common` covers console preferences and `help all` restores the full view.
`?` accepts the same topics. Help is bus-silent and remains available during work.

```text
help
color off
discover
end
model tmp112
addr 0x48
begin
config
read
mode shutdown
start
poll
tryread
threshold 20 30
alert interrupt
polarity low
faults 2
watch 20 1000
stop
health
```

Startup defaults to TMP102, address 0x48, continuous mode and 4 Hz. TMP102/TMP112 have no unique device-ID register: address ACK and configuration plausibility cannot identify the installed model. Select the model from the BOM. `model tmp112d` selects the X2SON address-select variant at 0x40–0x43 without physical ALERT; fixed-address ALERT variants use `model tmp112` at 0x48–0x4B. Discovery checks all eight candidate addresses.

Startup, `begin/init`, setting changes, `recover` and `shutdown` schedule
cooperative operations. Each loop polls at most one transfer for that operation,
including across settling intervals. Wait for the terminal result before sending
the next setting command. `job` shows cached progress/staged settings; `result`
shows the last terminal result, automatically collected by the CLI. `measure`
(`request`) schedules a read: latest temperature in continuous mode, a fresh
one-shot in shutdown mode. `cancel` and `stop` cancel an active operation without
I2C. A possible partial write remains dirty until explicit recovery. Cached
diagnostics and the input parser remain available while an operation runs.

## Manual owner-operation checks

Automatic polling is enabled by default. `job auto 0` pauses automatic owner
polling so a host or operator can inspect one exact step. `job step [budget]`
calls the owner with 0..255 transport callbacks (default 1) and reports the actual
count. Zero checks deadlines without I2C. `job start init|recover|shutdown|read
[timeout_ms]` accepts an explicit relative deadline of 1..2147483647 ms; normal
commands continue to use their existing deadlines. Use a long enough budget for
manual typing. Cached `job`, `result`, `health`, `settings`, `bus` and counter
queries remain available. `job auto 1` resumes automatic polling.
This switch controls owner jobs started directly from commands. Watch, scans
and diagnostics retain their normal scheduling, including full-test profile
restoration. Changing the switch or manually stepping during those workflows
returns BUSY.

For a healthy sensor, first run `mode shutdown` with automatic polling and wait
for its successful terminal result. Then use this sequence:

```text
job auto 0
xfer_reset
job start read 60000
job step 0
xfer_assert 0
job step 1
xfer_assert 1 1 0
job step 1
xfer_assert 2 2 0
job
```

The two transfers read CONFIG and trigger OS. Wait until the reported conversion
poll time, then step once to check readiness and once to acquire TEMP. Each step
must report at most one transfer; the terminal result must report a confirmed
one-shot sample. `xfer_assert 4 4 0` checks the normal completed sequence. If a
transfer fails, retain the error and counters rather than treating this count as
proof of a successful conversion. Restore `job auto 1` before automatic tests.

To test cancellation, start an operation in manual mode, step to the intended
point, capture `xfer_stats`, then run `cancel` and `result`. Cancellation must add
no transfers. A cancelled operation that may have written leaves configuration
dirty; enable automatic polling and run `recover` before managed sampling.
`invalidate` explicitly invalidates local state without a bus transaction, for
testing an externally reset or replaced sensor. It clears the CLI sample and
requires recovery; it does not send a device or shared-bus reset.

`xfer_assert <attempts> [<ok> <fail>]` compares the adapter's cached totals with
exact expected values and emits PASS or FAIL. A saturated counter cannot prove
an exact count. Adapter traffic includes probes, raw access and scans; driver
health counts only tracked sensor transfers. These commands make host assertions
possible without resetting or inferring driver health from adapter totals.

## Sampling and diagnostics

`read` returns the latest sensor register, including possible stale/reset data. Continuous `watch` may report the same conversion repeatedly. Shutdown-mode `watch` schedules one-shots, polls without blocking the CLI, and stops on a 500 ms completion timeout. Every watch/stress run is finite, accepts `stop`, and has bounded input work per loop. A stopped or timed-out watch issues no further background I2C. A pending one-shot remains available to `tryread` or a later watch; a transient read error does not discard it. Manual `start` requires explicit `poll`/`tryread` or joining a watch. Synchronous `readblocking` is explicitly diagnostic and accepts a bounded deadline.

`stress_mix [N]` runs 1..100000 finite cycles (default 100), with separate probe,
live configuration, threshold and temperature phases and a 100 ms pause between
cycles. Each tick performs one bounded phase; the threshold check uses up to three
transfers. It preserves sensor settings and uses the same mode-aware sampling
and stop behavior as watch. Register reads can acknowledge interrupt ALERT.
Summaries distinguish completed cycles, successful/failed checks and acquired
samples; only samples contribute to temperature statistics.

Integer arguments use decimal, including leading zeros, or explicit `0x` hex.
Spaces and tabs separate arguments. Overlong lines and unexpected control bytes
discard the whole line; backspace/DEL edits the current valid line. Ended-state
`addr`/`model` changes update the bus-silent binding immediately, so subsequent
`probe` and `recover` use the target shown by `cfg`.

`health` reads cached driver state, dirty-state cause, pending/ready conversion flags and independent adapter counters. Scans/probes affect adapter statistics but not driver health. OFFLINE is diagnostic; explicit operations can restore READY. `cfg` shows desired settings, not hardware readback. A validated setting is retained when a write fails ambiguously; health reports dirty configuration and `recover` explicitly reapplies it. Raw writes also dirty managed configuration. `end`, `bind` and `unbind` do not touch the bus; `shutdown` explicitly changes sensor power mode. Shared-bus reset is available only through the explicit maintenance command described below.

Any sensor-register read can acknowledge the ALERT latch in interrupt mode,
including config, dump, health-independent probe and verification reads.
`alertpin/intpin` samples only the optional application GPIO and reports assertion
using the configured polarity. It requires `TMP1X2_ALERT_PIN` and an ALERT-capable
model; default examples configure no ALERT input.

Additional diagnostics:

| Commands | Behavior |
| --- | --- |
| `verbose [0|1]`, `quiet [0|1]` | Control per-sample output; cached samples and run statistics still update |
| `stats [reset]` | Cached finite-run progress, elapsed time, errors, temperature min/max/mean and health/adapter deltas; reset after sampling stops |
| `xfer_stats/counters`, `xfer_reset` | Independent adapter counters, including scan/probe/raw traffic; idle reset preserves driver health |
| `xfer_assert <attempts> [<ok> <fail>]` | Exact cached transfer assertion; no I2C; saturated counters fail exact assertions |
| `bus` | Cached pins, bus clock and timeout; `version`/`settings` show framework and target |
| `invalidate` | Bus-silent external-change notification; clears cached CLI sample and requires recovery |
| `rawread <0..3>`, `rawwrite <1..3> <raw16>`, `rawdump` | Bound-only raw register access without tracked health; writes retain dirty-state uncertainty |
| `timing`, `convert <raw16>` | Conversion bounds/rates and pure temperature-register decoding |
| `thcalc <C> [normal/extended]`, `thdecode <raw16> [normal/extended]` | Pure threshold encoding/decoding without I2C |
| `tempf`, `raw`, `status_raw` | Managed Fahrenheit acquisition, tracked TEMP word, and CONFIG word diagnostics |
| `freshness [max_age_ms]` | Cached sample age/provenance and trust; no I2C, no claim of a new continuous conversion |
| `settings values` | Accepted setting values, formats and threshold ranges without I2C |
| `settings read/live` | Cooperative live configuration/threshold comparison against desired settings |
| `snapshot read` | Five-read CONFIG-bookended register snapshot with explicit temperature trust |
| `healthreset` | Idle reset of cumulative tracked totals only; preserves live state and last fault |

Run summaries preserve first/last errors and identify saturated counters as lower
bounds. Successful register transactions and acquired samples are distinct from
the driver's tracked physical-transfer counters.

## Comprehensive test workflows

`scan` performs 112 address-only probes from 0x08 through 0x77, one per tick.
It prints each ACK and each transport error other than address NACK. An ACK
proves an address responded; it does not identify a chip. Scan does not change
mux routing or the selected target.

`discover` (`detect`) checks the eight possible sensor addresses,
0x40..0x43 and 0x48..0x4B. It probes each address, then reads CONFIG only after
an ACK, with at most one callback per tick. Separating the phases lets native
IDF distinguish absence from a failed register transaction. It uses the same
resolution/reserved fixed-bit checks as the driver's `probe`. Each address reports the raw word or
transport status and whether that word is a plausible TMP1x2 register map.
Readable but incompatible words, absent addresses and other transport errors
are separate outcomes. A plausible word does not prove the chip family,
TMP102 versus TMP112, ordering code, package, wiring or measurement accuracy.
Confirm those from the BOM and hardware; discovery never selects a model/address.
The completion summary records `checked`, `ack`, `readable`, `candidates`,
`incompatible`, `absent` and `errors`. A complete run uses `8 + ack` callbacks.
Cancellation can leave an ACK whose CONFIG read has not yet run, so `checked`
counts completed address outcomes only. Errors remain failures even when other
addresses contain plausible candidates.

`job` reports cached diagnostic progress and `result` reports the last
completion. `stop`/`cancel` end scanning/discovery without further bus traffic.
Both leave driver health, desired settings, configuration trust and the cached
CLI sample unchanged; their physical transactions count in `xfer_stats`.
Discovery's CONFIG reads may acknowledge interrupt ALERT on responding devices.

`selfcheck` or bare `selftest` runs a cooperative read-only sequence with
pass/fail/skip counts: interface plausibility, configuration, thresholds,
temperature and optional physical GPIO. Reads may acknowledge interrupt ALERT.
An unavailable GPIO is a skipped check. This workflow does not establish sensor
accuracy or prove a new continuous-mode conversion.

`selftest full` explicitly changes sensor configuration through 18 verified
profiles covering every conversion rate, both modes/formats, both thermostat
modes, both polarities, all fault queues and representative threshold windows.
It captures the original desired profile and restores it at the end. Test
thresholds are chosen to remain representable when normal format is exercised.
The result reports test outcomes and restoration separately; register verification
does not replace physical thermostat or accuracy testing.

During the full test, the first `stop`/`cancel` cancels the active case and schedules
bounded baseline restoration. Polling continues while restoring. A second cancel
aborts restoration, retains the baseline as the desired profile and requires
explicit recovery. If restoration cannot reach hardware, that failure remains
visible; the CLI does not claim the old hardware profile was restored. A last-
resort bus-silent baseline rebind starts a new driver-health session, while the
diagnostic result and adapter counters retain the failure evidence.

## Explicit shared-bus maintenance

`busreset` (or `reset all`) sends general-call reset to **every compatible device
on the bus**. Bare `reset` does not issue it. Stop work on other affected bus
targets before using this command. The CLI invalidates its own device before
the attempt, including on ambiguous transfer failure; initialize/recover this
device and every other affected driver afterward. No initialization, self-test,
stress or recovery command sends a general-call reset implicitly.

`ara` / `alertresponse` performs one receive-only SMBus Alert Response transaction.
It reports the raw byte, winning address and status bit. Cause decoding is used
only for the selected known model/address, because TMP102 and TMP112 publish
different low-bit mappings. This acknowledges the winning device's interrupt;
it does not automatically drain all responders. Address-select TMP112D supports
this protocol despite lacking a physical ALERT pin.

Both commands affect adapter counters and bypass per-device tracked health.
See [BusOperations](../docs/bus-operations.md) for framing, uncertainty and exact
model-specific semantics.

Actual hardware and native-IDF build validation are reported in the repository validation notes; successful host tests do not establish those claims.
