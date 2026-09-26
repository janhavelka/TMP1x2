# Chip features and sibling-library parity

This matrix maps the TMP102/TMP112 interface to the library and diagnostic CLI.
The comparison covers the sixteen I2C libraries in [the sibling inventory](library-comparison.md).
It separates physical chip features, application-owned bus behavior, and reusable
software helpers so that a similarly named command does not imply unsupported
hardware behavior.

## Chip interface

| Feature | Library access | CLI / validation |
| --- | --- | --- |
| Signed normal/extended temperature, including negative values | `readSample`, `readTemperature`, `decodeTemperature`, raw register access | `read`, `measure`, `convert`; exhaustive signed-code host tests |
| Continuous conversion at 0.25, 1, 4, 8 Hz | `setConversionRate`, `setMode`, cooperative configuration | `rate`, `mode`, finite `watch` and stress |
| Shutdown and explicit one-shot | `shutdown`, `startShutdown`, `startOneShot`, `isConversionReady`, `tryRead`, `startRead` | `shutdown`, `oneshot`, `ready`, `tryread`, `measure` |
| Low/high thresholds and extended threshold encoding | `setThresholds`, `readThresholds`, `encodeThreshold`, `decodeThreshold` | `threshold`, `thcalc`, `thdecode`; rounding/bounds/readback tests |
| Comparator/interrupt thermostat behavior | `setAlertMode`, configuration readback | `alert`; full configuration exercise plus physical thermal/ALERT procedure |
| Active-low/active-high ALERT and comparator AL status | `setAlertPolarity`, `readConfiguration` | `polarity`, `config`; AL/POL host combinations |
| One, two, four, six consecutive faults | `setFaultQueue`, configuration readback | `faults`; full configuration exercise |
| Physical ALERT level | Optional application `gpioRead`, `readAlertPin` | `alertpin` / `intpin`; opt-in board input |
| All four registers and read-only/fixed fields | Typed access, tracked/raw words, decoded snapshots | `reg`, `wreg`, `dump`, `rawread`, `rawwrite`, `rawdump` |
| Classic and fixed addresses 0x48..0x4B | `Model::TMP102`, `Model::TMP112` and model-address validation | `model`, `addr`, `discover` |
| TMP112D ADD0 X2SON addresses 0x40..0x43 | `Model::TMP112D_ADDRESS_SELECT`; physical ALERT capability separated from internal thermostat/ARA behavior | `model tmp112d`, eight-address discovery; address/capability host tests |
| General-call software reset | Explicit `BusOperations` write to 0x00 with payload 0x06 | Explicit bus diagnostic; never part of initialization or recovery |
| SMBus Alert Response | Explicit receive-only transaction at 0x0C; raw responder/status and model-aware cause decoding | Explicit ARA diagnostic; acknowledges the winning responder |
| I2C high-speed mode | Application transport must issue the HS master code, repeated START and correct bus timing | Not synthesized by changing the example's 400 kHz clock setting |
| SMBus serial-interface timeout | Autonomous chip behavior; callbacks must honor transfer timeouts | Physical stuck-line validation; no programmable timeout register |
| Pointer persistence / repeated START | Pointer-write plus two-byte combined read callback | Framing tests and adapter builds; register auto-increment is not assumed |

The read-only resolution bits are fixed at `11`; they are not a programmable
resolution selector. Extended mode changes encoding range, not measurement
accuracy. Continuous mode has no sample counter or universal data-ready flag.
The chips provide no identity register, CRC, EEPROM, hardware offset calibration,
FIFO or per-address reset command. Those operations from other sensors therefore
have no TMP102/TMP112 register equivalent.

The relevant specification sections are TMP102
[SBOS397I](https://www.ti.com/lit/ds/symlink/tmp102.pdf), sections 6.3 and 6.5, and
TMP112 [SBOS473L](https://www.ti.com/lit/ds/symlink/tmp112.pdf), sections 7.3 and 7.5.
The unchanged [reference archive](reference/README.md) retains the PDFs, source
URLs, revisions and checksums.

## Initialization and common names

Most sibling sensors expose `begin(Config)` for synchronous initialization;
owner-driven libraries also expose `bind`, `startInitialize` and polling. Some
memory/GPIO libraries make `begin` an alias for binding because their device
initialization differs. TMP1x2 preserves its established verified initialization
semantics and offers compatible convenience names; it does not silently turn
`begin` into a bus-silent operation.

| TMP1x2 entry point | Contract / peer precedent |
| --- | --- |
| `bind(config)` | Bus-silent transport/profile admission; ADS1115, OPT4001, INA228, LDC1614 |
| `begin(config)`, `init(config)` | Synchronous apply and verified initialization |
| `begin()`, `init()` | Reinitialize the existing binding and desired profile |
| `startInitialize`, `startInit` | Bus-silent cooperative admission, then explicit budgeted polling |
| `end`, `unbind` | Local release; hardware shutdown/reset is explicit |
| `getSettings`, `getLastSample` | Cached copies, matching OPT4001/BME280/SHT3x/INA228 naming where applicable |
| `readRegister16`, `writeRegister16` | Word-register convenience names used by ADS1115/OPT4001 |
| `getThresholds`, `getConversionTimeMs` | Live threshold access and conservative execution-time bound, following ADS1115/OPT4001 names |
| `invalidateDeviceState` | Explicit local invalidation after outside hardware changes, following BME280 |

See [owner operations](owner-operations.md) for deadline, cancellation, staged
configuration and exactly-once result rules. Status enum ordinals and callback
signatures are not a common binary ABI across sibling libraries.

## Field helpers and diagnostics

Cached sample helpers distinguish an acquisition with a valid timestamp from an
unknown epoch. A recent cached timestamp also does not prove a newly completed
continuous conversion. Health snapshots separate transport outcomes from
configuration trust and sample provenance. Statistics reset does not turn OFFLINE
into READY or erase a recorded transport fault.

Pure configuration validation/encoding, expected register words, threshold bounds,
temperature/count/unit conversions, rate/fault-count helpers and register
snapshots are available without adding platform dependencies. Snapshot CONFIG
bookends detect changing persistent configuration; they cannot make a stale EM
transition payload trustworthy.

The shared CLI provides finite sampling/mixed stress, quiet mode, cached results,
cooperative scan/discovery/self-check, an explicit full configuration exercise
with baseline restoration, raw diagnostics and shared-bus maintenance. Details
and physical side effects are listed in [the example guide](../examples/README.md).
Host tests validate modeled behavior; the [hardware procedure](hardware-validation.md)
is still required for electrical timing, ALERT thresholds and sensor accuracy.
