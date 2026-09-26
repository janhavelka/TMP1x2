# Cooperative owner operations

Use `bind()`, `start*()`, `poll()` and `takeResult()` when a shared-bus scheduler
must control each transfer. The synchronous `begin()`, setters, `recover()` and
measurement helpers remain available for compatibility and diagnostics.

An instance accepts one operation at a time. Admission validates parameters and
returns `IN_PROGRESS` with a nonzero token without accessing I2C. Rejected starts
leave the caller's token unchanged. Tokens are not reused during the instance's
lifetime, including across unbind/rebind; exhaustion rejects further admission.

| Start method | Preconditions / work performed by later polls |
| --- | --- |
| `startInitialize(nowMs, timeoutMs, token)` | Bound transport; apply and verify desired configuration |
| `startConfigure(desired, nowMs, timeoutMs, token)` | Initialized; stage validated same-binding settings, apply and verify them |
| `startRecover(nowMs, timeoutMs, token)` | Bound transport; reapply desired settings, including dirty/failed initialization |
| `startShutdown(nowMs, timeoutMs, token)` | Initialized; apply and verify shutdown with required settling |
| `startRead(nowMs, timeoutMs, token)` | Initialized and trusted configuration; read latest continuous register or complete a shutdown one-shot |

`startConfigure` accepts a `Config` copied from `getConfig()`. Transport, address,
model, timing, offline threshold and GPIO bindings cannot be changed by a configuration job: use an
explicit bus-silent rebind for those. The staged target is visible through
`getOperationSnapshot().desiredConfig`. Before the first mutating write attempt,
cancellation preserves the previous desired profile. After that attempt, the
new desired profile remains available for recovery even if the callback fails.

## Polling and time

`poll(nowMs, maxTransfers)` defaults to one physical callback. A register read
is one combined write/repeated-start/read callback. CPU-only transitions and
wait checks do not consume the transfer budget. A zero budget allows deadline
checking without I2C. Poll never sleeps, yields, allocates or retries a failed
transfer. It returns `PollResult` with the status, actual transfer count, terminal
flag and next useful polling time.

`timeoutMs` is a relative whole-operation budget of 1 through `INT32_MAX`, starting
at admission. All supplied times use one wrapping monotonic millisecond domain.
When configured, `Config::nowMs` is authoritative and deadlines are checked again
after callbacks. Each callback receives no more than the remaining operation
budget or the configured I2C timeout. Operation timeout is `OPERATION_TIMEOUT`;
it is separate from a physical transport timeout and does not increment health
failure counters by itself.
If a callback finishes after the deadline, the operation reports timeout even
when that callback also failed. Its physical failure still appears in health
and `lastError()`; a late successful read does not publish a sample.

Without a clock hook, the owner supplies time through `poll(nowMs)`. Completion
time inside a synchronous callback cannot be observed; deadline decisions use
the supplied timestamps, and the sum of callback timeouts granted within one
poll cannot exceed its remaining budget. A settling wait starts at the first
poll after the triggering write, ensuring that an unobserved callback duration
cannot shorten TI's old-format/new-format conversion intervals. This conservative
schedule may add one owner tick to each wait. Use the clock hook when precise
post-callback deadline observations are required.

## Completion, cancellation and trust

Polling a completed job retains its terminal result. `takeResult(token, result)`
retrieves it exactly once without I2C. The return value reports successful
retrieval; **check `result.status` for the operation outcome**. Only a successful,
timely READ sets `result.hasSample`. Failed retrieval leaves `result` unchanged.
`TOKEN_MISMATCH` and `RESULT_NOT_AVAILABLE` distinguish a wrong identity from
absence of a consumable result.

While a job or terminal result is pending, competing managed calls, raw bus
access, probe, synchronous configuration and rebind return BUSY. Cached snapshots
and health getters remain available. `tick()` does not secretly advance an owner
job; only `poll()` does that work.
An unfinished manually started one-shot also blocks rebind, recovery and owner
admission. Complete it explicitly, or use `end()` to abandon it while preserving
the settling evidence needed by subsequent initialization/recovery.

`cancel()` is bus-silent and retains a CANCELLED result. A possible partial write
keeps configuration dirty and preserves required EM refresh/settling evidence.
The result's `hardwareEffectPossible` records an attempted mutating write, not
proof that it reached silicon. Sensor reads can separately acknowledge interrupt
ALERT. Cancellation never resets the bus or promises to abort a conversion
already running in the sensor.

`end()` cancels an active job, retains its terminal result and binding, and
deinitializes managed access without bus traffic. Consume that result before
restarting. `unbind()` explicitly discards binding, results and uncertainty
evidence; it does not change hardware. Callback contexts must remain alive while
the instance is bound.

## Scheduler example

The application creates the bus and supplies callbacks in `config`. Keep the
driver and token alive between scheduler calls:

```cpp
TMP1x2::TMP1x2 sensor;
TMP1x2::OperationToken token = 0;

auto status = sensor.bind(config);  // no I2C
if (status.ok()) {
  status = sensor.startInitialize(applicationNowMs(), 500, token); // no I2C
}
```

From the owning task, once per scheduler turn:

```cpp
if (sensor.operationActive()) {
  const auto progress = sensor.poll(applicationNowMs(), 1);
  if (progress.done) {
    TMP1x2::OperationResult result;
    const auto retrieved = sensor.takeResult(token, result);
    if (retrieved.ok()) {
      handleOperationStatus(result.status);
      if (result.hasSample) consumeTemperature(result.sample.celsius);
    }
  }
}
```

After consuming successful initialization, admit `startRead()` with a new token
and drive it the same way. Continuous mode still returns the latest register:
neither the job token nor its timestamp proves a new physical conversion. Use
shutdown one-shots for explicitly requested conversions.
