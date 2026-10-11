# Hardware validation checklist

Native tests validate the implementation against a modeled register device.
They do not establish physical accuracy, bus timing or ALERT pin behavior.
The [serial HIL runner](hil-runner.md) now automates the repeatable CLI checks,
records JSON/transcript evidence and restores the captured desired profile.
Run it separately for each BOM/package, strap, board revision and framework.
Its PASS is limited to selected checks; the physical gates below still apply.

1. Confirm the exact part/package and supply rating against its datasheet.
   Use common ground and appropriate SDA/SCL pull-ups. Set board pins explicitly.
2. Select the correct startup `TMP1X2_MODEL` and `TMP1X2_I2C_ADDRESS`, then build
   and flash the Arduino or native ESP-IDF CLI for the selected board. Initialization
   must not target another peripheral at the default 0x48 address on a shared bus.
   Capture `version`, `help`, `scan`, `config`, `health` and `dump` output.
3. Validate all four ADD0 address straps (GND, supply, SDA, SCL) independently:
   0x48..0x4B for classic parts, 0x40..0x43 for TMP112D address-select X2SON.
   Check fixed-address packages only at their specified address.
4. Compare room-temperature and negative-temperature measurements with a known
   calibrated reference, allowing thermal settling and recording uncertainty.
   Check normal and extended decoding at the **same safe physical temperatures**,
   including normal-to-extended-to-normal transitions under stable conditions.
   The wider signed register ranges are encoding ranges, not temperature ratings.
   Exercise values above 127.9375 C using pure `convert`/threshold encoding tests;
   never heat the device above the selected part's rated operating range merely
   to demonstrate the extended representation.
5. Exercise all four continuous conversion rates, shutdown and one-shot. Confirm
   that one-shot completion comes from OS and that shutdown holds the last value.
6. Test low/high limits, comparator and interrupt behavior, both polarities and
   all four fault-queue settings on ALERT-capable parts. Enable the example ALERT
   input and compare `alertpin` against an instrument. Record effects of register
   reads on ALERT. The TMP112D address-select package has no physical ALERT output.
   `--suite alert` automates stable comparator stimulus at every fault queue and
   polarity, plus GPIO agreement when wired. Verify exact N-conversion debounce,
   transition timing, hysteresis and interrupt acknowledge behavior separately
   with an instrument; register reflection alone does not establish them.
7. Disconnect/reconnect the sensor and confirm DEGRADED/OFFLINE/READY transitions,
   bounded callbacks, error detail, and explicit recovery. Include a second
   peripheral to verify that sensor recovery does not reset the bus.
8. Test actual scheduler locking and callback timeout enforcement. Record firmware
   version, board, chip marking, framework version, pull-ups and clock frequency.
9. Exercise cooperative `init`, `extended`, `shutdown`, `measure`, `job`, `cancel`
   and `recover`. Use a logic analyzer to verify the per-poll transfer budget and
   both EM settling intervals, including cancellation after the first write.
   Keep a second peripheral active between polls to check owner scheduling.
10. Run finite `stress` and `stress_mix` with verbose and quiet output, inject a
    disconnect, and compare run summaries with `health` and `xfer_stats`. Confirm
    `stop` produces no later background I2C and `xfer_reset` preserves health.
    Automated `--suite stress` checks completed counts and adapter bus silence;
    `--suite disconnect` provides timed operator windows and verifies tracked
    OFFLINE behavior, health-bypassing probes and explicit recovery.
11. Capture `settings` and `snapshot read`, then run `selfcheck` and
    `selftest full`. Verify all 18 configuration cases and final restoration.
    Cancel during EM settling, and disconnect during restoration. Confirm the
    terminal report distinguishes a verified restore from a retained desired
    baseline that still needs recovery. Repeat with extended-only baseline limits.
12. On a bus whose affected targets are all under test, issue `busreset` and
    inspect power-up register values with raw diagnostics. Verify other compatible
    targets also reset; explicitly recover every affected driver. Inject an
    ambiguous write failure and confirm local trust remains invalidated.
13. Configure interrupt ALERT on known TMP102 and TMP112 devices, cross both
    thermostat limits under controlled conditions, and issue `ara`. Record raw
    response, address, configured model/POL, decoded cause and physical GPIO.
    Test both polarities because the published TMP102/TMP112 status mappings
    differ. With two alerting responders, verify lowest-address arbitration and
    that one command acknowledges only one winner. Repeat protocol testing on
    address-select TMP112D using its internal event state without an ALERT pin.

No board has been flashed or physical validation claimed during this audit.
Archive the runner report and transcript alongside thermal-reference results,
oscilloscope/logic-analyzer traces, firmware build identity, exact chip marking,
schematic/PCB revision, supply, pull-ups and bus loading. Leave unperformed gates
marked NOT_RUN, including ARA and general-call reset, which the runner never sends.

## Bench record and command completion

Use one record per model/package, address strap, board and framework. Attach the
build/flash commands, firmware `version` output, `bus`, wiring, reference
equipment identity and uncertainty, observed values and the allowed limits.
Record PASS, FAIL or NOT_RUN for each procedure below. A blank or missing
instrument trace is NOT_RUN, not a pass inferred from host tests.

Before changing anything, save `settings`, `health`, `snapshot read` and
`job auto`. Wait for each matching `Operation result: token=...` and terminal
`OK` before sending another hardware command. A start prompt or IN_PROGRESS
message is not completion. At the end, restore the captured profile, then check
`verify`, `snapshot read` and `health`. Select extended format before restoring
thresholds outside the normal register range. `recover` restores the current
desired profile; it does not remember the profile from before a manual test.

The automated `negative`, `registers` and `owner` suites cover parser rejection,
raw/tracked accounting and bounded owner polling in addition to the original
smoke/configuration/one-shot/stress suites. See [the runner guide](hil-runner.md).
They do not replace the instrumented procedures below.

## Shutdown hold and one-shot completion

At a stable temperature, run:

```text
mode shutdown
measure
sample
rawread 0
```

Record the raw temperature word after the confirmed one-shot. Change the fixture
temperature within the selected part's rated range and verify the change with
the external reference. Wait for thermal settling, then issue `rawread 0`
several times. The word must remain unchanged in shutdown. Do not use `measure`,
`readblocking`, `watch` or `stress` during this hold check: each can start a new
shutdown conversion. A steady room-temperature sample alone cannot prove hold.

```text
start
ready
tryread
sample
```

Repeat `ready` until `Ready: yes`, within the chosen host deadline, then consume
with `tryread`. The final sample must have `one-shot=confirmed` and follow the new
reference temperature within its documented uncertainty. It is valid for the
first `ready` to report yes: serial latency may exceed a conversion. Record an
early not-ready observation only if actually observed. Check SCL/SDA with a logic
analyzer to establish the OS write, completion check and temperature-read order.
Repeat with extended format and with `readblocking 500`.

## Interrupt ALERT acknowledge and re-arm

Use an ALERT-capable package with a configured GPIO and an external reference.
The example values below require a fixture that can safely settle below 20 C,
between 20 and 30 C, and above 30 C. Otherwise choose two limits within the
available safe range. Start from a known initial device state and finish all
configuration/readback before applying the thermal stimulus:

```text
mode cont
threshold 20 30
extended 0
rate 8
polarity low
faults 1
alert interrupt
verify
alertpin
```

Warm through the high limit and observe the pin using only `alertpin` and the
instrument. After it asserts, run:

```text
alertpin
reg 0
alertpin
```

The register read acknowledges the interrupt; the pin must release. Keep the
temperature above the high limit and verify it stays released, then cool below
the low limit. A second interrupt must assert, remain latched and clear on
another `reg 0`. Repeat the complete high/acknowledge/low/acknowledge cycle at
both polarities and fault queues 1, 2, 4 and 6.

Record physical voltage as well as the CLI active/inactive result. Do not insert
`config`, `read`, `dump`, `probe`, `snapshot read`, `selfcheck` or typed setters
between assertion and acknowledgement: their register reads can clear the
evidence. Cached `health`, `job` and `settings` and GPIO-only `alertpin` do not
read the chip. The CONFIG AL bit reports comparator state even in interrupt
mode; it does not establish that the physical interrupt latch is asserted.
TMP112D ADD0 has no ALERT output, so mark this physical-pin procedure NOT_RUN for
that package and record its internal ARA behavior separately.

## Hysteresis, fault counts and conversion timing

Set `alert comparator` and known low/high limits. At each polarity, warm above
the high limit, then return to a temperature strictly between the limits. ALERT
must remain asserted. Cool below the low limit and return between the limits;
ALERT must remain released. Capture the external reference temperature and pin
transitions together. Threshold readback alone does not prove hysteresis.

Repeat conversion-rate settings `rate 0.25`, `rate 1`, `rate 4` and `rate 8` with
fault queues 1, 2, 4 and 6. Establish a stable temperature crossing with adequate
margin over reference uncertainty. Measure the time from the applied stimulus
to the ALERT edge, account for the unknown phase of the continuous conversion
cycle, and compare against the selected datasheet's timing limits. Include a
shorter crossing followed by a return to the in-window temperature to test that
faults must be consecutive. A slow thermal ramp cannot establish an exact
conversion count unless the reference resolves each crossing adequately.

The chip provides no continuous sample counter. Repeated identical temperature
words and CLI acquisition timestamps cannot measure its conversion rate.
Use a controlled fixture and instrument trace; leave exact timing NOT_RUN when
the fixture cannot distinguish sensor timing from thermal or serial latency.
Capture EM transitions on SDA/SCL to verify that the old-format conversion is
allowed to settle and a new-format conversion completes before trust returns.

## SMBus Alert Response and arbitration

Use the interrupt setup above. Once an interrupt is observed, issue `ara`
instead of `reg 0`, then `alertpin`. Capture the receive-only transaction at
7-bit address 0x0C, the response byte and the pin release. The response address
must equal the actual alerting target; its least significant bit is a status bit,
not an I2C R/W bit to discard. Compare the decoded cause for the selected BOM
model and polarity with the known high or low thermal event. Repeat both events
and both polarities for TMP102 and TMP112: their published status-bit mappings
differ. The [reference archive](reference/README.md) contains the exact datasheet
revisions used by the library.

Do not issue a target register read between the thermal event and `ara`; that
would acknowledge the event before the ARA test. After acknowledgement and
before the opposite limit is crossed, a further ARA with no responders should
NACK rather than return a fabricated address. An I2C error by itself does not
prove which device acknowledged, and an ambiguous receive failure may already
have cleared the event. Re-establish the stimulus before retrying a failed case.

For arbitration, configure two known responders at distinct addresses before
stimulating either. Use active-low ALERT for a shared wired-OR line, or observe
separate physical pins. With both interrupts asserted, the first `ara` must
return the lower address and acknowledge only that device. The second must
return the remaining responder; inspect both pins or the shared line throughout.
The CLI decodes a cause only when the responder equals its selected, trusted
target. An undecoded response from the other address is expected. Record both
identities explicitly. Repeat on ADD0 TMP112D using captured ARA responses,
without claiming a physical ALERT-pin result.

## General-call reset and state invalidation

Run this only on a fixture where resetting every compatible device is part of
the test. Save every affected application's desired settings. Use non-default
thresholds/rate first so reset has an observable effect, then run:

```text
settings
busreset
health
rawdump
```

The trace must show address 0x00, write direction and payload 0x06. Check the
power-up persistent configuration fields (normal format, continuous 4 Hz,
comparator, active low, one fault), TLOW=0x4B00 and THIGH=0x5000. CONFIG normally
starts at 0x60A0; account for live OS/AL status rather than requiring dynamic bits
to equal a fixed value at an arbitrary later time. TEMP initially reads zero and
then updates after the automatic startup conversion; serial latency can miss the
zero interval, so absence of a captured zero is not a reset failure by itself.
The driver must retain dirty configuration evidence even though raw reads work.

Issue `recover`, wait for its terminal result, then `verify` and `snapshot read`.
Recover every other affected driver explicitly and verify their profiles too.
With an instrumented transport fault, repeat a reset that may have reached the
device but whose callback reports failure. Local trust must remain dirty and
ordinary reads must not silently certify the old configuration. Record the
actual callback error separately from the observed device effect.

For an external power/reset change performed by a different owner, `invalidate`
provides the bus-silent local notification. Check that `xfer_stats` is unchanged,
`health` reports dirty and explicit recovery is still required. Do not use a
healthy transport state as proof of configuration trust.

## Owner budgets, cancellation and shared-bus scheduling

The automatic `owner` suite verifies zero/one/two transfer budgets, cancellation
before and after a write, deadline expiry and invalidation. For physical evidence, connect
a logic analyzer and keep a second peripheral active under the real application's
bus owner. Step a job manually:

```text
job auto 0
job start shutdown 60000
job step 0
job
job step 1
job
```

Zero-budget polls and cached `job` reports must produce no I2C. Each one-budget
poll must use at most one physical callback. Repeat `job step 1` until
`write-attempted=yes`, then issue `job cancel`. Record the bus counters immediately
before and after cancellation; it must add no transfer, and trust must retain
the possible hardware effect. Wait for the matching CANCELLED result, restore
`job auto 1`, then explicitly `recover`. For EM settling cancellation, use
`job auto 0` and `extended 1` (or `extended 0` from extended baseline), step to the
first write and cancel. Ordinary typed commands retain their default 1000 ms
deadline, so automate the command sequence if manual typing would exceed it.

For deadline expiry, run `job auto 0`, `job start read 1`, wait more than 1 ms,
and `job step 0`. Expect OPERATION_TIMEOUT and zero new I2C; restore `job auto 1`.
Compare callback durations to their passed timeout and show that other owners run
between polls. Desktop tests cannot prove transport timeout enforcement, RTOS
locking, mux selection or other-device progress on the deployed node.

Finally, cancel `selftest full` during an EM transition. The first `stop` schedules
baseline restoration and may therefore produce later I2C; wait for the diagnostic
summary and `Baseline restoration: verified`. On a separate run, disconnect
during restoration, and on another issue a second `stop` while restoring. Both
must report restoration not verified and retain the original desired profile for
later recovery. Restore connection/automatic polling and reinitialize explicitly.
