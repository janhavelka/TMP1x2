# Serial hardware validation runner

`tools/hil_tmp1x2_runner.py` exercises the **same shared CLI** in the Arduino and
native ESP-IDF examples. It was inspired by the sibling OPT4001 and ADS1115 serial
runners, with completion tracking for this library's cooperative operations.
It requires a source checkout: tooling is intentionally omitted from the library
distribution archive. Python parser tests and the native CLI model do not count
as hardware validation. No physical TMP1x2 run is claimed by this change.

## PCB and firmware prerequisites

1. Confirm the complete orderable part and package from the BOM. `tmp112` includes
   the classic and fixed-address ALERT packages; `tmp112d` means specifically the
   X2SON ADD0 package at 0x40..0x43. Ordering-code spelling alone is insufficient.
   These devices have no unique readable part identifier. A plausible CONFIG
   value or ACK cannot prove identity.
2. Verify supply, ground, local decoupling, ADD0/address strap, SDA/SCL pull-ups,
   GPIO voltage compatibility and bus loading against the selected datasheet.
   Provide accessible SDA, SCL, ground, supply and optional ALERT test points.
   For shared-bus testing, provide a way to disconnect only the sensor without
   removing the bus pull-ups or driving an unpowered device through its pins.
3. Build the CLI with the **correct startup model and address**, before attaching
   it to a populated shared bus. Set `TMP1X2_MODEL=0` (TMP102), `1` (TMP112), or
   `2` (TMP112D ADD0), plus `TMP1X2_I2C_ADDRESS` as needed. Model-aware defaults are
   0x48 for models 0/1 and 0x40 for model 2. Set `TMP1X2_I2C_SDA`,
   `TMP1X2_I2C_SCL` and optional `TMP1X2_ALERT_PIN` to the actual board pins. The
   examples initialize their compiled target on startup; selecting `--model` or
   `--address` in the host runner only checks that target, never retargets it.
   In particular, do not let the default 0x48 initialization touch an ADS1115 or
   another peripheral at that address. See [examples](../examples/README.md).
4. Build using `scripts\pio.cmd run -e esp32s3dev` or `esp32s2dev` on Windows,
   or the documented native ESP-IDF example build. Flash using your usual board
   procedure. The runner does not build, flash, scan, change address or send a
   general-call reset. Close other serial monitors before opening the port.
5. Let the board reach a stable temperature. Use a reference thermometer when
   evaluating accuracy. For the `oneshot` suite, the normal/extended/normal
   samples must remain within `--format-tolerance-c` (default 1 C); keep the
   sensor away from a hand, airflow changes and switched heat sources. The broad
   default `--min-c -40 --max-c 125` is a plausibility window, **not** an accuracy
   claim or authorization to exceed the selected device's rating. Set a narrower
   window appropriate to the fixture, for example 15..35 C at room temperature.
6. Complete initialization and leave the CLI idle with trusted configuration and
   no pending manual one-shot. Preflight rejects existing work. Avoid running any
   other owner or CLI client concurrently. Opening a serial port can reset some
   boards despite the runner setting DTR/RTS inactive before opening; the captured
   baseline is the profile observed **after** connection and startup.

Use Python 3.10 or newer. Install the only hardware dependency in the Python
environment used for the runner, not in a second PlatformIO Core:

```powershell
python -m pip install pyserial
python tools/hil_tmp1x2_runner.py --dry-run --suite smoke --suite config --suite oneshot --suite stress
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp102 --address 0x48 --board "PCB rev A / ESP32-S3" --part "exact BOM ordering code" --operator "Honza" --notes "3.3 V, pull-ups 4.7k, 400 kHz, reference thermometer ID" --min-c 15 --max-c 35 --suite smoke --suite config --suite oneshot --suite stress --stress-count 100
```

Run the same command after flashing the native IDF example and record each
framework/build separately. A host GPIO test is useful only when the example has
the GPIO callback enabled and the trace actually reaches the package's ALERT pin.

## Suites and side effects

| Suite | Checks | Device effects |
| --- | --- | --- |
| `smoke` (default) | Firmware version, target/BOM selection, configuration/threshold verification, coherent snapshot, nine selfchecks, raw/count/Celsius consistency, acquisition clock and temperature window | Reads only after example startup; reads can acknowledge interrupt ALERT |
| `config` | All 18 firmware configuration cases plus verified restoration; compare every captured desired setting after the test | Changes all configurable fields, including thermostat mode, polarity, thresholds and EM; can toggle ALERT |
| `oneshot` | Shutdown conversion completion, confirmed provenance and coherent snapshots in normal, extended, then normal format; compare the temperatures to catch coherent but stale format decoding | Changes mode, thresholds and EM; triggers conversions; restores original profile in cleanup |
| `stress` | Finite sample and mixed probe/config/threshold/sample runs, exact completed/sample counts, no transport failures during each run, stable adapter counters after stop | Shutdown sampling triggers one-shots; finite continuous reads can repeat a conversion |
| `alert` | Comparator release/assert/release sequence at both polarities and all four fault queues; compare raw CONFIG/AL with expected state and physical GPIO when configured | Sets continuous 8 Hz, extended format, comparator, polarity, fault queue and thresholds around measured ambient; deliberately toggles ALERT |
| `disconnect` | Timed operator disconnect/reconnect; tracked failures reach configured OFFLINE threshold, probes preserve health, explicit recovery returns READY | Operator physically disconnects only the sensor; runner changes to continuous mode and explicitly recovers |

The comparator sequence is **release, assert, release**. It uses windows 5..10 C
above or below measured ambient and waits longer than the requested fault queue.
It establishes stable response, not an exact fault-count or conversion-period
measurement. Do not attach the tested ALERT output to an actuator or system reset
input. The `config` and `oneshot` suites also change thermostat conditions; run
them only where those changes are acceptable.

The optional suites are selected explicitly:

```powershell
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp112 --address 0x49 --suite alert --min-c 15 --max-c 35
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp102 --address 0x48 --suite disconnect --fault-window 30
```

For `disconnect`, follow the console's two bounded windows: disconnect after the
first instruction, reconnect after OFFLINE is observed. No external relay or
operator shell command is executed. A missing stimulus is INCONCLUSIVE; an
incomplete serial response remains INCONCLUSIVE even if its partial text contains
an expected I2C failure. A second active peripheral and a logic analyzer are
needed to establish shared-bus isolation and callback timing physically.

## Completion, cleanup and evidence

The runner waits for a terminal operation with its matching token, the complete
diagnostic summary or the exact finite-run count. A prompt printed when work
starts is not success. Every serial read/write is bounded; defaults are 5 seconds
per ordinary command and 120 seconds for a full diagnostic or finite run. Increase
`--long-timeout` for long stress runs (each mixed cycle has a nominal 100 ms gap,
plus transfer/conversion time). Missing, truncated, malformed or unsupported
responses are INCONCLUSIVE. No retry silently changes a failed check to success.

Startup capture and each command response are limited to 8 MiB. Reaching this
limit stops that check as INCONCLUSIVE and still allows cleanup. The runner
accepts at most 32 suite selections and stops after 10,000 recorded checks, with
room reserved for cleanup. Each configurable time window is at most 86,400
seconds. Disconnect polling and retained one-shot polling use the time remaining
in their outer window as the command deadline.

Mutating suites save the original desired profile. In `finally`, cleanup sends
one `stop`, verifies that owner work is idle, and consumes any retained manual
one-shot within a bounded window. It then recovers and restores all settings with
typed commands. Extended mode is selected before restoring potentially
extended-only threshold values; original mode and format are reinstated. Final
configuration verification, snapshot and health must pass. A full-test
cancellation waits for the firmware's own baseline restoration; a second cancel
would abort that restoration and is not sent automatically. A normal first
Ctrl-C also attempts cleanup. Killing the process, losing power or disconnecting
the serial cable can prevent cleanup.

If restoration cannot be verified, the overall run fails, the original profile
is retained in JSON and manual recovery is required. Stop the external stimulus,
reconnect and inspect the captured settings before restoring them through the
CLI. Do not assume that a failed or cancelled write left hardware unchanged.
Health totals, cached samples, conversion history and interrupt latch history
are not restored: this is desired configuration restoration, not time reversal.

Each run creates a timestamped `.json` report and adjacent `.txt` transcript under
`hil_logs/` (ignored by git), or a new path selected by `--report`. Both paths are
reserved before opening the serial port; existing evidence is not overwritten.
A `.json.lock` file prevents two runner processes from claiming the same report.
After an interrupted process, keep the partial evidence and choose a new report
path. The JSON includes source commit/dirty status, firmware
version response, board/part/operator notes, selected suite outcomes, captured
profile, every command's response/duration/outcome, restoration status and manual
gates left `NOT_RUN`. The raw transcript is written during the run and retains
ANSI bytes and startup output. An initial INCONCLUSIVE report remains if the
process stops before finalization; its `hardware_run` is `null` because an
interrupted connection state is unknown. A completed setup-failure report uses
`false`. The final JSON replaces the initial report atomically.

The JSON retains up to 16,777,216 characters of command output. Older output
beyond that limit is removed from JSON with a note pointing to the full transcript;
check outcomes and reasons remain. Serial setup failures are recorded even when
the port never opens. A transcript write error stops normal checks and allows
cleanup. Transcript write or close failures cannot produce a successful exit.
A stale reservation lock
after a completed report produces a warning and does not change the test result;
choose a new report path for the next run. For an evidence write failure,
preserve the available files and console output when the runner cannot finalize
the report.

| Outcome | Meaning |
| --- | --- |
| `PASS` | The selected automated check met its explicit expectations |
| `FAIL` | Observed contradictory behavior or unverified required restoration |
| `SKIP` | Optional observed capability unavailable, such as an unconfigured ALERT GPIO |
| `INCONCLUSIVE` | Incomplete evidence, setup/transport framing failure or missing operator stimulus |
| `NOT_RUN` | Selected suite not reached after an earlier failure, or a separate manual gate |

Exit codes: **0** selected checks passed, **1** a check failed, **2** inconclusive
or setup failure. Optional GPIO SKIP does not fail register-only suites. PASS is
limited to the selected automated checks; it does not certify thermal accuracy,
interrupt ALERT/ARA semantics, exact timing, electrical margins or HIL coverage
on another part/package/framework. Follow the remaining
[hardware validation procedure](hardware-validation.md).

## Host regression tests

```powershell
python tools/test_hil_tmp1x2_runner.py
cmake -S . -B build -DTMP1X2_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python tools/test_hil_tmp1x2_runner.py --cli-fixture build/tmp1x2_cli_tests.exe
```

The native fixture reuses the existing CLI register model. It validates actual
shared CLI output, reordered suites, cancellation after a write, retained
shutdown one-shots and restoration. It does not emulate board electrical behavior
or physical ALERT timing and never creates hardware PASS evidence.
See the [testing guide](testing.md) for generator-specific executable paths and
the full host, framework and CI procedures.
