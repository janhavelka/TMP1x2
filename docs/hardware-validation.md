# Hardware validation checklist

Native tests validate the implementation against a modeled register device.
They do not establish physical accuracy, bus timing or ALERT pin behavior.

1. Confirm the exact part/package and supply rating against its datasheet.
   Use common ground and appropriate SDA/SCL pull-ups. Set board pins explicitly.
2. Build and flash the Arduino or native ESP-IDF CLI for the selected board.
   Capture `version`, `help`, `scan`, `config`, `health` and `regs` output.
3. Validate all four ADD0 address straps (GND, supply, SDA, SCL) independently.
4. Compare room-temperature and negative-temperature measurements with a known
   reference. Check normal and extended decoding and a value above 128 C only
   with suitable equipment within the part's operating limits.
5. Exercise all four continuous conversion rates, shutdown and one-shot. Confirm
   that one-shot completion comes from OS and that shutdown holds the last value.
6. Test low/high limits, comparator and interrupt behavior, both polarities and
   all four fault-queue settings. Record effects of register reads on ALERT.
7. Disconnect/reconnect the sensor and confirm DEGRADED/OFFLINE/READY transitions,
   bounded callbacks, error detail, and explicit recovery. Include a second
   peripheral to verify that sensor recovery does not reset the bus.
8. Test actual scheduler locking and callback timeout enforcement. Record firmware
   version, board, chip marking, framework version, pull-ups and clock frequency.

No board has been flashed or physical validation claimed during repository setup.
