# Integration and ownership

The core is standard C++17 and has no Arduino, ESP-IDF, RTOS, logging or heap
dependency. Include `TMP1x2/TMP1x2.h`. The application supplies synchronous
`i2cWrite` and `i2cWriteRead` callbacks in `Config`, plus an opaque `i2cUser`.
Each callback must perform one bounded transfer attempt; a register read is one
pointer-write/repeated-START/read transaction, with the 16-bit value MSB first.
Callbacks must complete before returning. Stack buffers are valid only during
the call. The supplied timeout covers lock acquisition and the entire transfer.

The application owns bus creation, destruction, pins, speed, locking, retries,
recovery and scheduling. The core never calls a bus begin/end/reset routine.
Keep the context and callbacks alive while the instance remains bound, until
`unbind()`, destruction, or replacement by a new binding. `end()` retains the
binding: probes, raw diagnostics and `recover()` can still call it afterward.
Serialize all calls on a
driver instance and on the shared bus. Calls are task-context APIs, not ISR-safe;
callbacks must not re-enter the same instance. `end()` releases local state; use
shutdown mode explicitly when the physical sensor must stop converting.

For shared-bus scheduling, use the [cooperative owner API](owner-operations.md):
bus-silent start methods followed by `poll(nowMs, maxTransfers)` and exactly-once
result retrieval. Initialization, configuration, recovery, shutdown and reads
all have owner operations. Waiting for conversion never holds the bus lock.
`tick(nowMs)`, `poll(nowMs)` and the optional clock callback must use the same
wrapping 32-bit monotonic millisecond domain. Transport callbacks themselves
remain bounded synchronous operations.

Validation errors and conversion-not-ready results are distinct from I2C
failures. Health is passive: READY, DEGRADED, OFFLINE and UNINIT describe observed
transport behavior. OFFLINE does not prevent an explicit retry. Recovery is
requested by the application and restores the cached desired settings; it does
not recover or reset the shared bus.

Raw register writes are diagnostics. They invalidate configuration trust;
recover the desired profile before relying on typed reads. A failed
write may have reached hardware, so ambiguous transfer errors also invalidate
trust. Multi-register updates must not be treated as atomic hardware operations.

Changing extended mode requires monotonic time: an application `nowMs` callback
for synchronous configuration, or caller-supplied poll timestamps for owner
operations. TI documents an
encoding transition where the mode marker can change before the temperature
payload. The synchronous configuration helper shuts down in the old format,
waits 36 ms (the 35 ms conversion bound plus clock quantization margin), applies
the new format and obtains a completed new-format one-shot before restoring the
desired mode. Budget at least 72 ms plus bounded transfers
for that operation, and supply `cooperativeYield` for your scheduler. An absent
or stalled clock produces an error; callers must not assume the mode marker alone
proves the payload format during a hardware transition.
An observed EM mismatch in live CONFIG or TEMP retains the fresh-conversion
requirement across subsequent failures, `end()`/`bind()` and matching CONFIG
readback. A TEMP marker mismatch first reports `MEASUREMENT_NOT_READY` and latches
dirty state; further managed reads require recovery. Changing desired EM to match
the observed hardware cannot bypass that conversion requirement.
Explicit `unbind()` forgets all state, including that evidence. A missing clock
can leave format evidence latched even if the operation failed before writing;
provide the clock and recover rather than trying to clear uncertainty by changing
the desired EM setting again.
Entering shutdown from continuous mode also requires a 36 ms settling interval
before a one-shot can start, using the clock hook or owner poll timestamps.
Same-format continuous operations
do not require a clock or temporary shutdown.

Synchronous configuration/recovery helpers use the same configuration state
machine as owner jobs. Each typed setter performs at most 11
transport callbacks; `begin()` and `recover()` perform at most 12. An EM refresh
adds two settling waits; entering shutdown without an EM refresh adds one.
Ordinary same-format configuration uses eight callbacks (nine for initialization
or recovery). Each callback has its own `i2cTimeoutMs` budget. Wait completion
depends on the application's monotonic clock and bounded yield callback; no
bus lock is held across a wait. A stalled clock is rejected after 1,000,000
unchanged polling iterations. Schedule these synchronous configuration calls
where that latency is acceptable.

Select the exact model/package from your BOM. TMP102 and classic/fixed-address
TMP112 use 0x48 through 0x4B. `Model::TMP112D_ADDRESS_SELECT` selects the X2SON
variant with ADD0 at 0x40 through 0x43 and no physical ALERT output; configuring
an ALERT GPIO for that variant is rejected. The address-select range and pin
difference follow [TI's TMP112 datasheet, Table 7-4 and pinouts](https://www.ti.com/lit/ds/symlink/tmp112.pdf).
All variants use the documented register interface. No unique device ID exists;
probe establishes interface plausibility only. No general-call reset or SMBus
alert-response transaction is sent implicitly.

In interrupt ALERT mode, register reads may acknowledge the latch. This includes
diagnostic dumps, verification and readiness polling. The AL configuration bit
describes comparator state, not a reliable copy of the interrupt latch. See the
[TI reference notes](reference/README.md) for the datasheet wording.
