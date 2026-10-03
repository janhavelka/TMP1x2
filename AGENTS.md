# TMP1x2 repository conventions

- Use simple engineering language in documentation, diagnostics and reports.
- After verification, commit the prompt's changes and push the intended branch
  after each prompt or logical block. Preserve unrelated work and confirm that
  the branch is synced with its upstream.
- Keep public headers under `include/TMP1x2/` and implementation under `src/`.
  The core is standard C++17: no Arduino, ESP-IDF, logging, bus ownership,
  platform delays, dynamic allocation or hidden retries.
- Follow the documented callback, Status, typed-enum and passive-health contracts.
  Driver health counts physical tracked transport attempts; configuration trust
  and failed preconditions are separate. Probes and raw APIs bypass health.
- Applications own I2C initialization, pins, serialization, clocks and recovery.
  Callbacks are synchronous, bounded by their timeout, and must not re-enter.
  `end()` is bus-silent; `shutdown()` is the explicit fallible device operation.
- Preserve configuration-dirty and pending format-refresh evidence after partial
  writes. TI's EM transition requires settling in the old format and a completed
  new-format conversion. The TEMP format marker alone is insufficient.
- Use `library.json` as the version source and regenerate with
  `python scripts/generate_version.py sync`; do not hand-edit `Version.h`.
- Use the existing VS Code-managed PlatformIO through `scripts/pio.cmd` on Windows.
  Do not install another PlatformIO Core.
- Arduino and native ESP-IDF use the shared framework-neutral example CLI. Keep
  its help layout, ANSI colors, command parsing and diagnostic semantics aligned.
- Run relevant native tests and framework-boundary checks after changes. Build
  ESP32-S2/S3 for public-header or adapter changes. Do not equate SDK-header
  compilation, mocks or CI configuration with a native IDF link or hardware test.
- TI reference artifacts retain their own licenses. Keep source URLs and hashes
  when changing the archive; reference code is not part of the library build.
