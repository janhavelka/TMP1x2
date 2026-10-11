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
   `job auto` must report `on`; the `owner` suite temporarily changes it and cleanup
   restores automatic polling before starting asynchronous profile restoration.

Use Python 3.10 or newer. Install the only hardware dependency in the Python
environment used for the runner, not in a second PlatformIO Core:

```powershell
python -m pip install pyserial
python tools/hil_tmp1x2_runner.py --dry-run --suite smoke --suite config --suite oneshot --suite stress
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp102 --address 0x48 --board "PCB rev A / ESP32-S3" --part "exact BOM ordering code" --operator "Honza" --notes "3.3 V, pull-ups 4.7k, 400 kHz, reference thermometer ID" --min-c 15 --max-c 35 --suite smoke --suite config --suite oneshot --suite stress --stress-count 100
```

For the full set of checks that need no external stimulus, select `smoke`,
`negative`, `registers`, `owner`, `config`, `oneshot` and `stress` explicitly.
They still modify the chip configuration; the default remains `smoke`.

`version` reports the flashed firmware's `Firmware: git=... tree=... built=...`
identity separately from the host checkout. The runner retains both identities.
To check the flashed image against a known clean build, add these options:

```powershell
$ExpectedCommit = git rev-parse HEAD
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp102 --address 0x48 --expected-firmware-commit $ExpectedCommit --expected-framework Arduino-ESP32 --expected-target esp32s3 --require-clean-firmware --suite smoke --suite negative --suite registers --suite owner --suite config --suite oneshot --suite stress
```

Use `native-esp-idf` for the native IDF framework and `esp32s2` for an S2 target.
Commit comparisons accept a matching 7..40-digit Git abbreviation because older
Arduino build metadata abbreviates revisions. A commit match alone does not
prove the source tree was clean; use `--require-clean-firmware` when that matters.
Without an expectation, unknown firmware fields remain unknown and verification
is recorded as `not requested`. The host checkout's commit never substitutes for
the flashed image's identity. Rebuild older firmware that lacks the identity line.

Run the same command after flashing the native IDF example and record each
framework/build separately. A host GPIO test is useful only when the example has
the GPIO callback enabled and the trace actually reaches the package's ALERT pin.

## Suites and side effects

| Suite | Checks | Device effects |
| --- | --- | --- |
| `smoke` (default) | Firmware version, target/BOM selection, configuration/threshold verification, coherent snapshot, nine selfchecks, raw/count/Celsius consistency, acquisition clock and temperature window | Reads only after example startup; reads can acknowledge interrupt ALERT |
| `config` | All 18 firmware configuration cases plus verified restoration; compare every captured desired setting after the test | Changes all configurable fields, including thermostat mode, polarity, thresholds and EM; can toggle ALERT |
| `oneshot` | Cooperative shutdown conversions in normal/extended/normal format, temperature continuity, manual `start`/`ready`/`tryread`, and `readblocking`; confirmed provenance and snapshots | Changes mode, thresholds and EM; triggers conversions; restores original profile in cleanup |
| `stress` | Finite sample and mixed probe/config/threshold/sample runs, exact completed/sample counts, no transport failures during each run, stable adapter counters after stop | Shutdown sampling triggers one-shots; finite continuous reads can repeat a conversion |
| `negative` | Reject malformed register, configuration, numeric and job inputs; verify unchanged settings, cached owner state, passive health and physical transfer counters | Inputs must cause no I2C; cleanup is prepared before sending them in case a faulty parser accepts a write |
| `registers` | All four raw/tracked registers, signed/extended pure conversions, exact raw-vs-tracked accounting, trust invalidation after a same-value raw threshold write, recovery | Rewrites the current low threshold, invalidates trust and explicitly recovers; restores entry profile at session cleanup |
| `owner` | Zero/one/two transfer budgets, cancellation before and after the first write, retained dirty/effect evidence, 1 ms deadline expiry without I2C, bus-silent invalidation and recovery | Temporarily disables automatic polling; starts/cancels shutdown and read jobs; invalidates and recovers configuration |
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
Selfcheck requires all snapshot trust flags, independently of its PASS count.
Finite stress must account for actual transfers: mixed cycles include one
health-bypassing probe each, while sample-only runs use tracked transfers only.
Zero work or inconsistent counters cannot pass on the strength of a summary.

Startup capture and each command response are limited to 8 MiB. Reaching this
limit stops that check as INCONCLUSIVE. The runner
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
would abort that restoration and is not sent automatically. A first Ctrl-C
between complete commands attempts cleanup. Interrupting an incomplete serial
command loses response framing and defers cleanup as described below. Killing
the process, losing power or disconnecting the serial cable can prevent cleanup.

If restoration cannot be verified, the overall run fails, the original profile
is retained in JSON and manual recovery is required. Stop the external stimulus,
reconnect and inspect the captured settings before restoring them through the
CLI. Do not assume that a failed or cancelled write left hardware unchanged.
Health totals, cached samples, conversion history and interrupt latch history
are not restored: this is desired configuration restoration, not time reversal.
If a firmware startup/reset banner appears during a command, the session is
INCONCLUSIVE and no automatic restoration writes are sent to the unchecked
target. If configuration had been changed, restoration is reported as unverified
and requires an operator to check the target and captured profile. This detects
reported restarts; an unreported reset or a different firmware that emits no
recognized banner still requires independent power/reset evidence.

A partial serial write, response timeout, oversized response, serial exception
or interruption during a command also prevents further commands, including
cleanup. The CLI might still hold a partial input token or finish the previous
operation later, so another prompt cannot safely establish command ownership.
Preserve the transcript, inspect the target through a new operator-controlled
serial session, finish or cancel any outstanding work, and recover the saved
profile explicitly. Read-only runs become INCONCLUSIVE; runs that may have
changed settings fail required restoration. A complete, framed command that
reports a semantic error still permits normal cleanup.

Each run creates a timestamped `.json` report and adjacent `.txt` transcript under
`hil_logs/` (ignored by git), or a new path selected by `--report`. Both paths are
reserved before opening the serial port; existing evidence is not overwritten.
A `.json.lock` file prevents two runner processes from claiming the same report.
After an interrupted process, keep the partial evidence and choose a new report
path. The JSON includes source commit/dirty status, firmware
version response, board/part/operator notes, selected suite outcomes, captured
profile, parsed firmware identity and requested-identity comparison, every
command's response/duration/outcome, restoration status and manual
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
shared CLI output, the negative/register/owner suites, reordered suites, cancellation after a write, retained
shutdown one-shots and restoration. It does not emulate board electrical behavior
or physical ALERT timing and never creates hardware PASS evidence.
See the [testing guide](testing.md) for generator-specific executable paths and
the full host, framework and CI procedures.
