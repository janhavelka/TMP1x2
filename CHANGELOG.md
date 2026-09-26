# Changelog

## Unreleased

### Fixed

- Preserve observed temperature-format uncertainty through partial observations,
  failures and recovery; adopting an observed EM setting requires a fresh sample.
- Keep CLI target bindings and desired settings consistent after rejected commands.
- Stop background I2C when a watch stops or times out, and retain pending one-shots
  after transient read failures.
- Accept tab-separated input and explicit decimal/hex integers; discard malformed
  control-character lines. Expose dirty-state causes and pending conversion state.
- Synchronize the CMake version, support renamed native-IDF component directories,
  and keep generated example artifacts out of release packages.

### Added

- Cooperative initialization, configuration, recovery, shutdown and read jobs,
  with bounded transfers per poll, operation deadlines, cancellation, staged
  desired settings and retained exactly-once results. Legacy synchronous APIs
  share the configuration state machine.
- TMP112D address-select model for 0x40..0x43, capability-aware ALERT GPIO
  validation, eight-address discovery and native IDF handle support.
- Shared CLI operation/result/cancel commands, raw register diagnostics,
  conversion/threshold helpers, finite mixed stress, quiet sampling, run statistics, transport-counter
  reset and optional physical ALERT sampling.
- Independent owner-operation failure/deadline/budget tests and exhaustive
  model/address validation.
- Managed PlatformIO native ESP-IDF S2/S3 example builds and unbuffered CLI console.
- Regression coverage for format provenance, CLI failure workflows, version sync,
  framework-free headers, IDF component naming and isolated release consumers.
- Cross-library audit and clarified callback lifetime/configuration timing contracts.

## 1.0.0 - 2026-09-22

### Added

- TMP102 and TMP112 typed register driver with normal/extended temperature decoding,
  conversion rates, shutdown/one-shot operation, thresholds and ALERT configuration.
- Framework-neutral transport injection, passive health tracking, explicit recovery
  and configuration verification.
- Arduino and native ESP-IDF diagnostic examples with shared CLI conventions.
- Native protocol/failure tests and archived TI reference material.
