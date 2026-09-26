# Field integration helpers

All core helpers use fixed-size values and application-owned callbacks. None
initializes a bus, allocates memory, logs output or retries failed transfers.

## Initialization and configuration

`init(config)` has the same verified synchronous behavior as `begin(config)`.
`init()` and `begin()` reuse the existing binding and desired settings through
recovery. They need the clock callback when the hardware transition requires
settling. Use `bind` plus `startInitialize` (`startInit`) and `poll` when the owner
must schedule each transfer. The [owner API](owner-operations.md) documents that
path's deadlines and result lifecycle.

`defaultConfig(model)` chooses the matching model/address defaults; transport
callbacks still belong to the application. `validateSettings` checks persistent
sensor settings without requiring callbacks. `validateConfig` also checks the
binding. Both are pure and leave the object untouched.

For expected readback or configuration tools, `encodeConfiguration(config, word)`
encodes persistent CONFIG bits with OS cleared. It cannot accidentally trigger a
one-shot. `expectedConfigurationRegister(config, reg, value, mask)` supplies the
expected CONFIG/TLOW/THIGH word and its stable comparison mask. Invalid inputs
leave outputs unchanged. Live OS/AL bits are excluded from CONFIG verification;
threshold reserved bits must remain zero.

`getMode`, `getConversionRate`, `getExtendedMode`, `getAlertMode`,
`getAlertPolarity`, `getFaultQueue` and threshold getters return cached desired
values. Use live reads or `verifyConfiguration` when hardware evidence is needed.
`getSettings` provides a cached copy with binding, trust and conversion state.
`getThresholds` is the peer-compatible live-read alias of `readThresholds`.

## Sample provenance and age

`Sample::timestampValid` says that a clock was available at acquisition; a valid
timestamp may be zero. `Sample::freshConversion` means the driver confirmed its
requested one-shot before that read. Continuous samples are latest-register
observations, even when their acquisition timestamps differ.

`getLastSample(out)` copies historical cached data without I2C. The overload
`getLastSample(out, nowMs, maxAgeMs)` also requires trusted state, a valid
acquisition timestamp and an age within the requested bound. Failure leaves
`out` unchanged. `sampleFresh(nowMs, maxAgeMs)` tests that same age/trust condition;
it does not claim continuous-mode conversion freshness. Use `freshConversion`
when the distinction matters.

All times use the same wrapping monotonic millisecond domain. Maximum accepted
age is `INT32_MAX` ms. The older `sampleAgeMs()` retains its epoch-relative
behavior for compatibility; check `sampleTimestampValid()` before using it to
make freshness decisions. Supplying a clock later cannot retroactively validate
an old unknown timestamp.

## Health and outside hardware changes

`healthSnapshot()` copies passive state, counters, consecutive failures, last
fault, timestamps and their validity flags without I2C. `resetStatistics()` clears
only cumulative success/failure counts. It preserves live state, consecutive
failures, last error, timing evidence, configuration trust and cached samples.
Applications should reset cumulative counters between measured runs to avoid
making a delta span the reset.

Call `invalidateDeviceState()` when another bus owner may have reset or changed
this device. It is bus-silent, invalidates cached acquisition/conversion state and
preserves conservative configuration/format uncertainty for recovery. It rejects
active operations or unconsumed results; complete/cancel and consume them first.
It does not reset transport health or send a physical reset.

An explicit shared-bus reset can affect several instances and device types.
Invalidate every affected local device before the attempt and perform explicit
initialization/recovery afterward, including when the transfer failed ambiguously.
See [shared-bus operations](bus-operations.md).

## Registers and units

`readRegister16` / `writeRegister16` are aliases of tracked word access. Raw access
remains explicitly separate and bypasses tracked health.

`readSnapshot(RegisterSnapshot&)` captures CONFIG, TEMP, TLOW, THIGH and CONFIG in
five bounded tracked reads. Matching CONFIG bookends detect only observed
persistent-field changes; they do not make the capture atomic. A failed transfer
or changed bookend preserves the caller's output, while retaining any observed
configuration uncertainty internally. Raw/decoded diagnostic values can be
reported with `temperatureTrusted == false`; such a value must not be used as a
trusted managed measurement. This diagnostic never updates the sample cache or
claims a fresh conversion.

`readTemperatureFahrenheit` and `readTemperatureCounts` use the normal managed
read contract. Pure helpers cover C/F conversion, signed 1/16-degree counts,
threshold representable limits/resolution, conversion rate in Hz, fault-queue
counts and the conservative maximum conversion duration. Encoding rejects
non-finite or out-of-range values and retains output on failure. Encoding bounds
do not expand the part's operating temperature or accuracy specifications.
`getConversionTimeMs` returns that execution-time bound; the conversion-rate
period is a separate value obtained from `conversionPeriodMs`.
