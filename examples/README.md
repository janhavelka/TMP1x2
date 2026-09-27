# Diagnostic examples

Both examples run the same framework-neutral command processor in `common/Tmp1x2Cli*.cpp`, split into command dispatch, diagnostics and output. Arduino and native ESP-IDF therefore have the same commands, aliases, help layout, ANSI colors, finite sampling workflows and parsing behavior. These are bring-up diagnostics, not a multitask bus-manager implementation.

`common/BoardConfig.h` selects SDA 8, SCL 9, 400 kHz and a 50 ms transaction timeout. Override `TMP1X2_I2C_SDA`/`TMP1X2_I2C_SCL` or edit this example-only file for your hardware. Optional `TMP1X2_ALERT_PIN` defaults to -1 (disabled); configure an input with appropriate external pull-up for physical ALERT diagnostics. Fit appropriate external I2C pull-ups. Platform pin setup and handles stay in the examples.

## Arduino / PlatformIO

Build `esp32s3dev` or `esp32s2dev` using the root `platformio.ini`. Open the 115200 baud monitor. The application creates and owns `Wire`; the driver receives only write and repeated-start read callbacks. Only `setup`/`loop` access the driver or I2C. Wire short-read errors retain generic I2C status because the returned byte count cannot prove which phase failed.

## Native ESP-IDF

From `esp_idf/basic`, with an activated ESP-IDF 5.3 or newer environment:

```text
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Use `esp32s2` for S2. The example uses the configured native IDF console; change console settings with `idf.py menuconfig` when using a USB console instead of UART.

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
on IDF I2C, timer, GPIO and task facilities.

## Typical session

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

`scan` and `discover` now probe one address per tick. `job` reports cached
progress and `result` reports the last completion. `stop`/`cancel` end these
read-only workflows without further bus traffic. Scan covers 0x08..0x77;
discovery covers only the eight possible sensor addresses. An ACK is not chip
identification; a transport error other than address NACK is reported separately.

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
