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
   procedure. The runner does not build, flash, change address or send a
   general-call reset. Only the explicit `detection` suite probes other legal
   sensor addresses. Close other serial monitors before opening the port.
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
   boards despite the runner setting DTR/RTS inactive by default; the captured
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

For Arduino ESP32-S2 using native USB CDC, add `--dtr 1 --rts 0`: its USB
transport requires asserted DTR before it can transmit. The defaults
`--dtr 0 --rts 0` suit the ESP32-S3 USB Serial/JTAG console and avoid deliberately
asserting reset/boot signals on UART bridges. Select the options for the actual
console route, not only the processor. Both lines are configured before opening
the port and are not toggled during the run. Keep RTS inactive on the S2 native
console; changing it can reset the target. The report records both options.

For the checks on the selected target that need no external stimulus, select `smoke`,
`negative`, `registers`, `owner`, `config`, `oneshot` and `stress` explicitly.
They still modify the chip configuration; the default remains `smoke`.
Add `--suite detection` to check all eight legal sensor addresses when CONFIG
reads at other responding devices are acceptable. It validates CONFIG fixed bits,
every address outcome, totals and physical transfer counts, while requiring the
selected profile and passive driver health to stay unchanged. CONFIG reads can
acknowledge interrupt ALERT. This establishes plausible candidates, never chip
identity, and it does not select a discovered device automatically.

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
| `detection` | All eight legal addresses, per-address CONFIG plausibility, complete totals, selected target present, exact adapter counts and unchanged passive health/profile | Address probes and CONFIG reads at every ACK; reads can acknowledge another device's interrupt ALERT; no automatic binding |
| `config` | All 18 firmware configuration cases plus verified restoration; compare every captured desired setting after the test | Changes all configurable fields, including thermostat mode, polarity, thresholds and EM; can toggle ALERT |
| `oneshot` | Cooperative shutdown conversions in normal/extended/normal format, temperature continuity, manual `start`/`ready`/`tryread`, and `readblocking`; confirmed provenance and snapshots | Changes mode, thresholds and EM; triggers conversions; restores original profile in cleanup |
| `stress` | Quiet finite sample and mixed probe/config/threshold/sample runs, exact completed/sample counts, no transport failures or new console loss, stable adapter counters after stop | Shutdown sampling triggers one-shots; finite continuous reads can repeat a conversion; original verbose setting is restored |
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

## Duration soak

Append a soak after the selected suites with `--soak-seconds`. For example,
this runs smoke checks followed by at least eight hours of mixed testing:

```powershell
python tools/hil_tmp1x2_runner.py --port COM7 --model tmp102 --address 0x48 --suite smoke --soak-seconds 28800 --soak-batch-count 20 --min-c 15 --max-c 35
```

The maximum duration is seven days (`604800` seconds). Each batch uses the
existing finite `stress_mix` command, with 20 cycles by default and a configurable
range of 1..1000. Every cycle probes the target and checks CONFIG, thresholds and
temperature. The runner verifies the completed cycle/sample counts, exact
raw-versus-tracked transfer accounting and absence of transport failures. After
each batch it checks the cached sample, trusted READY health and console loss
counters. A batch counts as completed only after all these checks pass. Continuous
sampling may read the same conversion more than once; shutdown sampling must
report confirmed one-shot provenance.

Duration uses the host monotonic clock and starts after quiet-mode setup. The
runner completes the current batch and its checks before stopping, so duration
can exceed the request by one batch plus those checks. `--long-timeout` bounds
each finite batch, not the whole soak. Allow for the nominal 100 ms gap per mixed
cycle plus transfers and any conversion waits when increasing batch size.
Progress prints at most once per `--soak-progress-seconds` (default 60), after a
complete batch. The first failure stops the soak; there is no retry or automatic
recovery inside the duration loop. Normal session cleanup still applies.

The JSON holds aggregate batch/cycle/sample counts, command outcomes and
minimum/mean/maximum command latency, with only a terminal result and any failed
command retained as ordinary checks. These latencies include serial transfer and
CLI work; they do not measure sensor conversion timing. Successful batches do not
consume the 10,000-check limit or grow retained results indefinitely. The raw
transcript streams to disk throughout the soak, so disk use still grows with
duration. An interrupted or incomplete duration is INCONCLUSIVE unless a check
observed a failure. A complete duration with zero verified batches cannot pass.

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

Preflight records cached `bus` settings and `console` counters. Historical dropped
or truncated console output is retained as the baseline. Neither counter may
increase during testing; a saturated loss counter cannot establish intact output
and is INCONCLUSIVE. Stress and soak check these counters after every batch, and
the runner checks them again at the end. An unsolicited `CONSOLE_OUTPUT_LOST`
error fails the affected command. Firmware without these metrics must be rebuilt.

Stress and soak capture `verbose`, select `verbose 0` to suppress per-sample
output, then restore the entry value during cleanup. This also applies to
continuous read-only runs. Quiet mode keeps large finite runs within the response
limit; their terminal summaries still contain all run and transfer counters.

Startup capture and each command response are limited to 8 MiB. Reaching this
limit stops that check as INCONCLUSIVE. The runner
accepts at most 32 suite selections and stops after 10,000 recorded checks, with
room reserved for cleanup. Ordinary configurable time windows are at most 86,400
seconds; the aggregate soak duration allows 604,800 seconds. Disconnect polling and retained one-shot polling use the time remaining
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
If quiet mode had been selected, its original value is also retained for manual
restoration when framing loss or a restart prevents further commands.

Each run creates a timestamped `.json` report and adjacent `.txt` transcript under
`hil_logs/` (ignored by git), or a new path selected by `--report`. Both paths are
reserved before opening the serial port; existing evidence is not overwritten.
A `.json.lock` file prevents two runner processes from claiming the same report.
After an interrupted process, keep the partial evidence and choose a new report
path. The JSON includes source commit/dirty status, firmware
version response, board/part/operator notes, selected suite outcomes, captured
profile, parsed firmware identity and requested-identity comparison, bus settings,
console loss baseline, ordinary command responses/durations/outcomes, aggregate
soak results, configuration and verbose restoration status, and manual
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
shared CLI output, detection and negative/register/owner suites, reordered suites, cancellation after a write, retained
shutdown one-shots, quiet stress, a short soak and restoration. Fake-clock tests
cover thousands of complete batches with bounded retained results, first-failure
termination, console loss and interrupted durations. These do not emulate board electrical behavior
or physical ALERT timing and never create hardware PASS evidence.
See the [testing guide](testing.md) for generator-specific executable paths and
the full host, framework and CI procedures.
