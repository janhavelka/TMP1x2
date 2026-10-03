# Changelog

## Unreleased

### Fixed

- Preserve configuration and temperature-format evidence from tracked word reads,
  including transient EM changes before subsequent recovery.
- Retain the observed low-threshold verification mismatch when reading the high
  threshold fails, separately from the transport failure.
- Preserve prior temperature-format evidence when synchronous setters or rebinding
  adopt an externally changed EM value; the matching marker can still hold an old payload.
- Retain an observed low-threshold mismatch when the following high-threshold read fails.
- Reject CLI setup replacement during active work and preserve the session on invalid
  replacement; clear stale input/results/statistics on successful idle reuse.
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

- Standalone CMake integration guidance and framework boundary checks covering
  the shared CLI as well as the core; no application repository is required.
- Example bus frequency, timeout and Arduino serial baud overrides, with
  transport-specific range checks.
- A TMP112D address-select regression for internal thermostat configuration
  without a physical ALERT pin.
- Build-time example model/address selection with capability checks, applied
  before initial I2C access on both Arduino and native ESP-IDF.
- Serial HIL suites with bounded command completion, baseline restoration,
  machine-readable results and transcripts; host tests also exercise the real
  shared CLI output. Physical execution remains pending.
- Byte-by-byte partial-write fault matrices and stalled hardware-conversion tests.
- Separate core and shared CLI implementation files by responsibility while retaining
  their public APIs and shared configuration engine; update all build/package paths.
- Complete chip/peer feature matrix and field-integration guide; consistent
  `init`, bound-profile initialization and common settings/sample/register names.
- Pure configuration validation/encoding and expected register words, unit/count
  helpers, cached sample timestamp/fresh-conversion provenance and age checks,
  health snapshots, statistics reset and explicit outside-change invalidation.
- Bookended live register snapshots with explicit temperature trust flags.
- Explicit shared-bus general-call reset and receive-only SMBus Alert Response,
  preserving raw status and model-specific alarm-cause semantics.
- Cooperative discovery/scan/self-check and a full configuration test with
  baseline restoration, plus field-oriented readback and freshness CLI commands.
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
