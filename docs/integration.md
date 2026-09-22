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
Keep the context and callbacks alive until `end()`. Serialize all calls on a
driver instance and on the shared bus. Calls are task-context APIs, not ISR-safe;
callbacks must not re-enter the same instance. `end()` releases local state; use
shutdown mode explicitly when the physical sensor must stop converting.

For shared-bus scheduling, start a one-shot in shutdown mode, return to your
scheduler, and poll with caller time. Waiting for conversion must not hold the
bus lock. `tick(nowMs)` and an optional clock callback must use the same wrapping
32-bit monotonic millisecond domain. The transport calls themselves are bounded
synchronous operations; this is not an asynchronous bus executor.

Validation errors and conversion-not-ready results are distinct from I2C
failures. Health is passive: READY, DEGRADED, OFFLINE and UNINIT describe observed
transport behavior. OFFLINE does not prevent an explicit retry. Recovery is
requested by the application and restores the cached desired settings; it does
not recover or reset the shared bus.

Raw register writes are diagnostics. They invalidate configuration trust;
recover the desired profile before relying on typed reads. A failed
write may have reached hardware, so ambiguous transfer errors also invalidate
trust. Multi-register updates must not be treated as atomic hardware operations.

Changing extended mode requires a monotonic `nowMs` callback. TI documents an
encoding transition where the mode marker can change before the temperature
payload. The synchronous configuration helper shuts down in the old format,
waits 35 ms, applies the new format and obtains a completed new-format one-shot
before restoring the desired mode. Budget at least 70 ms plus bounded transfers
for that operation, and supply `cooperativeYield` for your scheduler. An absent
or stalled clock produces an error; callers must not assume the mode marker alone
proves the payload format during a hardware transition.
Entering shutdown from continuous mode also requires that clock and a 35 ms
settling interval before a one-shot can start. Same-format continuous operations
do not require a clock or temporary shutdown.

TMP102 and TMP112 cannot be distinguished by reading an ID register: neither has
one. Select the model from your BOM. Both use addresses 0x48 through 0x4B. A probe
shows only that a compatible-looking register responds. No general-call reset
or SMBus alert-response transaction is sent implicitly.

In interrupt ALERT mode, register reads may acknowledge the latch. This includes
diagnostic dumps, verification and readiness polling. The AL configuration bit
describes comparator state, not a reliable copy of the interrupt latch. See the
[TI reference notes](reference/README.md) for the datasheet wording.
