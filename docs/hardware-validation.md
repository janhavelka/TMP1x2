# Hardware validation checklist

Native tests validate the implementation against a modeled register device.
They do not establish physical accuracy, bus timing or ALERT pin behavior.

1. Confirm the exact part/package and supply rating against its datasheet.
   Use common ground and appropriate SDA/SCL pull-ups. Set board pins explicitly.
2. Build and flash the Arduino or native ESP-IDF CLI for the selected board.
   Capture `version`, `help`, `scan`, `config`, `health` and `dump` output.
3. Validate all four ADD0 address straps (GND, supply, SDA, SCL) independently:
   0x48..0x4B for classic parts, 0x40..0x43 for TMP112D address-select X2SON.
   Check fixed-address packages only at their specified address.
4. Compare room-temperature and negative-temperature measurements with a known
   reference. Check normal and extended decoding and a value above 128 C only
   with suitable equipment within the part's operating limits.
5. Exercise all four continuous conversion rates, shutdown and one-shot. Confirm
   that one-shot completion comes from OS and that shutdown holds the last value.
6. Test low/high limits, comparator and interrupt behavior, both polarities and
   all four fault-queue settings on ALERT-capable parts. Enable the example ALERT
   input and compare `alertpin` against an instrument. Record effects of register
   reads on ALERT. The TMP112D address-select package has no physical ALERT output.
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
