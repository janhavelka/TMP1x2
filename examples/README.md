# Diagnostic examples

Both examples run the same framework-neutral command processor in `common/Tmp1x2Cli.cpp`. Arduino and native ESP-IDF therefore have the same commands, aliases, help layout, ANSI colors, finite sampling workflows and parsing behavior. These are bring-up diagnostics, not a multitask bus-manager implementation.

`common/BoardConfig.h` selects SDA 8, SCL 9, 400 kHz and a 50 ms transaction timeout. Override `TMP1X2_I2C_SDA`/`TMP1X2_I2C_SCL` or edit this example-only file for your hardware. Fit appropriate external I2C pull-ups. No pins or platform handles enter the library.

## Arduino / PlatformIO

Build `esp32s3dev` or `esp32s2dev` using the root `platformio.ini`. Open the 115200 baud monitor. The application creates and owns `Wire`; the driver receives only write and repeated-start read callbacks. Only `setup`/`loop` access the driver or I2C. Wire short-read errors retain generic I2C status because the returned byte count cannot prove which phase failed.

## Native ESP-IDF

From `esp_idf/basic`, with an activated ESP-IDF 5.3 or newer environment:

```text
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Use `esp32s2` for S2. The example uses the configured native IDF console; change console settings with `idf.py menuconfig` when using a USB console instead of UART.

The application creates one `i2c_master` bus and four fixed device handles for addresses 0x48–0x4B. Creating handles does not probe the device. An input task queues characters with backpressure; only `app_main` calls the command processor, driver and bus. There is no Arduino API or compatibility facade in this example. The root component has no framework dependencies; the example depends on IDF I2C, timer, GPIO and task facilities.

## Typical session

```text
help
color off
discover
end
model tmp112
addr 0x48
begin
config
read
mode shutdown
start
poll
tryread
threshold 20 30
alert interrupt
polarity low
faults 2
watch 20 1000
stop
health
```

Startup defaults to TMP102, address 0x48, continuous mode and 4 Hz. TMP102/TMP112 have no unique device-ID register: address ACK and configuration plausibility cannot identify the installed model. Select the model from the BOM.

`read` returns the latest sensor register, including possible stale/reset data. Continuous `watch` may report the same conversion repeatedly. Shutdown-mode `watch` schedules one-shots, polls without blocking the CLI, and stops on a 500 ms completion timeout. Every watch/stress run is finite, accepts `stop`, and has bounded input work per loop. Synchronous `readblocking` is explicitly diagnostic and accepts a bounded deadline.

`health` reads cached driver state and independent adapter counters. Scans/probes affect adapter statistics but not driver health. OFFLINE is diagnostic; explicit operations can restore READY. `cfg` shows desired settings, not hardware readback. A validated setting is retained when a write fails ambiguously; health reports dirty configuration and `recover` explicitly reapplies it. Raw writes also dirty managed configuration. `end`, `bind` and `unbind` do not touch the bus; `shutdown` explicitly changes sensor power mode. There is no general-call reset command.

Any sensor-register read can acknowledge the ALERT latch in interrupt mode, including config, dump, health-independent probe and verification reads. The CLI does not configure an ALERT GPIO. An application can separately inject the optional GPIO sampler without changing bus ownership.

Actual hardware and native-IDF build validation are reported in the repository validation notes; successful host tests do not establish those claims.
