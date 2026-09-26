# Explicit shared-bus operations

Include `TMP1x2/BusOperations.h` for general-call reset and SMBus Alert Response
(ARA). These commands affect the application-owned bus independently of any one
driver instance. They are never sent by `begin`, `init`, polling or recovery.
Each helper makes one bounded synchronous callback, with no allocation, delay,
retry, driver-health update or hidden follow-up transaction.

## General-call reset

`BusOperations::generalCallReset(write, user, timeoutMs)` sends one byte, `0x06`,
to seven-bit address `0x00`. The callback uses the normal `I2cWriteFn` contract.
Timeout must be 1..`INT32_MAX` ms. Missing callbacks or invalid timeouts cause no
traffic. The callback must return a terminal status; `IN_PROGRESS` is normalized
to a transport error while retaining its detail value.

This resets every compatible device on the bus, potentially including devices
from other libraries. A successful ACK cannot establish which targets reset;
an error cannot establish that none reset. Quiesce all affected device operations,
cancel and consume any retained jobs, and invalidate local driver state before
issuing the command. Explicitly initialize/recover those devices afterward.

For a TMP1x2 instance the sequence is:

```cpp
// After the application has stopped work on every affected bus target:
auto status = sensor.invalidateDeviceState(); // local state only
if (status.ok()) {
  status = TMP1x2::BusOperations::generalCallReset(
      applicationWrite, &applicationBus, 50);
  reportResetTransfer(status); // recovery remains necessary after an error
}
// The owner subsequently admits and polls explicit recovery for each target.
```

The helper cannot enumerate or invalidate other driver objects. The caller must
perform that work, including after an ambiguous transfer failure. Reset restarts
conversion state; a power-up TEMP value must not be treated as a fresh result.

## SMBus Alert Response

`BusOperations::readAlertResponse(receive, user, timeoutMs, response)` reads one
byte from seven-bit address `0x0C`. `ReceiveFn` must perform a receive-only
transaction: START, address+read, one byte, final NACK, STOP. There is no register
pointer write. Return OK only after the complete requested byte count arrives.

The result preserves `raw`, the responder's seven-bit `address`, and
`alertStatusBit`. The low bit is status, not a fixed R/W bit. The pure
`decodeAlertResponse` helper rejects reserved or impossible responder addresses;
other valid unicast devices remain valid generic responses. No address proves
device identity. Failed reads/decodes leave the output unchanged.

An ARA transaction acknowledges the lowest-address winning responder's interrupt
alert. Other responders may remain pending. One helper call obtains one winner;
there is no automatic drain loop. Even an error can follow an acknowledgement
that already changed a physical latch. TMP112D address-select packages support
ARA despite having no physical ALERT output.

Only interpret cause when the application knows the responder's model and
configured polarity:

```cpp
TMP1x2::BusOperations::AlertResponse response;
auto status = TMP1x2::BusOperations::readAlertResponse(
    applicationReceive, &applicationBus, 50, response);
if (status.ok() && response.address == config.i2cAddress) {
  TMP1x2::BusOperations::AlertCause cause;
  status = TMP1x2::BusOperations::decodeAlertCause(
      response, config.model, config.alertPolarity, cause);
  if (status.ok()) handleAlertCause(cause);
}
```

The published model mappings differ. TMP102 defines a high-threshold cause when
the status bit equals POL; TMP112 describes status one as high-threshold and
status zero as low-threshold without a polarity qualification. The decoder follows
those model-specific descriptions and retains the original byte for inspection.
The result describes the acknowledged event, not the current comparator AL bit
or GPIO level. Sources: TI TMP102
[SBOS397I section 6.3.7](https://www.ti.com/lit/ds/symlink/tmp102.pdf) and TMP112
[SBOS473L section 7.3.2.5](https://www.ti.com/lit/ds/symlink/tmp112.pdf).

## Transport features

The main register callback already exposes complete chip settings using explicit
pointer writes and full two-byte words. Remembered-pointer reads and single-byte
register access are alternate transfer forms, not additional sensor controls.

High-speed I2C requires an HS master-code sequence and correct electrical timing;
it belongs in a capable application transport. The example adapters use ordinary
400 kHz transfers. The chip's autonomous SCL-low serial timeout likewise has no
programmable register. Neither behavior can be established by host mocks; verify
them with the intended controller and physical hardware if used.
