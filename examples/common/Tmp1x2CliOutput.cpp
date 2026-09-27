#include "Tmp1x2Cli.h"

// Shared help and cached views; rendering itself performs no bus operations.

namespace tmp1x2_cli {
const char* Cli::color(unsigned code) const {
  if (!_color) return "";
  switch (code) {
    case 31: return "\033[31m";
    case 32: return "\033[32m";
    case 33: return "\033[33m";
    case 36: return "\033[36m";
    case 90: return "\033[90m";
    default: return "\033[0m";
  }
}
void Cli::printVersion() {
  print("%sTMP1x2 diagnostic CLI%s | %s %s | %s | built %s %s\n", color(36), color(0),
        _platform.framework, _platform.frameworkVersion, _platform.target, __DATE__, __TIME__);
#ifdef TMP1X2_VERSION_STRING
  print("Library: %s\n", TMP1X2_VERSION_FULL);
#endif
}
void Cli::printHelp() {
  print("%s=== TMP1x2 CLI Help ===%s\n", color(36), color(0));
  const auto section = [this](const char* title) { print("\n%s[%s]%s\n", color(32), title, color(0)); };
  const auto item = [this](const char* command, const char* description) {
    print("  %s%-32s%s - %s\n", color(36), command, color(0), description);
  };
  section("Common");
  item("help / ?", "Show this help");
  item("version / ver", "Firmware, library and framework version");
  item("scan", "Probe 0x08..0x77; bounded, diagnostic traffic");
  item("discover", "Probe 0x40..0x43 and 0x48..0x4B; identity unverified");
  item("init / begin", "Start cooperative initialization and verification");
  item("bind / unbind", "Bus-silent transport binding or full release");
  item("end", "Release driver state without changing the bus");
  item("shutdown", "Cooperatively apply low-power shutdown mode");
  item("addr [address]", "Set model-valid target address while ended");
  item("model [tmp102|tmp112|tmp112d]", "BOM/package selection; tmp112d is ADD0 X2SON");
  item("color [0|1|off|on]", "Show or toggle ANSI colors");
  item("verbose [0|1] / quiet [0|1]", "Show or set per-sample watch/stress output");
  section("Data");
  item("read / temp", "Read latest register; freshness not guaranteed");
  item("tempf", "Read latest temperature in Fahrenheit");
  item("measure / request", "Start cooperative sample operation");
  item("start / oneshot", "Trigger single conversion in shutdown mode");
  item("poll / ready", "Check one-shot completion");
  item("tryread", "Read pending one-shot when ready");
  item("readblocking [timeout_ms]", "Bounded one-shot convenience, 1..5000 ms");
  item("sample / sampleage", "Show cached CLI sample and age without I2C");
  item("freshness [max_age_ms]", "Cached acquisition age and conversion provenance");
  item("watch [N] [interval_ms]", "Finite sampling, default 20 at 1000 ms");
  item("stop", "Stop sampling or cancel an operation; no I2C");
  item("job / job status", "Cached cooperative operation progress; no I2C");
  item("result / job result", "Retained terminal operation result; no I2C");
  item("cancel / job cancel", "Cancel active operation without I2C");
  item("stats [reset]", "Cached run summary or idle summary reset; no I2C");
  item("timing", "Conversion timing, resolution and register ranges");
  item("convert <raw16>", "Decode a TEMP register word without I2C");
  item("thcalc <C> [normal|extended]", "Encode/decode a rounded threshold without I2C");
  item("thdecode <raw16> [format]", "Decode a threshold in normal or extended format");
  section("Configuration");
  item("cfg / settings / snapshot", "Desired settings cached by CLI; no I2C");
  item("settings read / settings live", "Cooperative live configuration/threshold checks");
  item("settings values", "Show accepted configuration field values");
  item("snapshot read", "Coherent CONFIG/TEMP/threshold register diagnostic");
  item("mode [cont|shutdown]", "Continuous conversion or low-power shutdown");
  item("rate [0.25|1|4|8]", "Conversion rate in Hz");
  item("extended [0|1|off|on]", "Select normal 12-bit or extended 13-bit format");
  item("threshold [low_C high_C]", "Read or set temperature thresholds");
  item("alert [comparator|interrupt]", "Thermostat operating mode");
  item("polarity [low|high]", "ALERT output polarity");
  item("faults [1|2|4|6]", "Consecutive faults before ALERT");
  item("alertpin / intpin", "Sample opt-in physical ALERT GPIO; no I2C");
  item("verify", "Compare hardware configuration with driver cache");
  section("Registers");
  item("config / status", "Read and decode live configuration register");
  item("status_raw / raw", "Tracked raw CONFIG / TEMP register word");
  item("dump", "Read all four registers; can clear interrupt ALERT");
  item("reg / rreg <0..3>", "Read one 16-bit register (MSB first)");
  item("wreg <1..3> <0..0xFFFF>", "Tracked register write; marks configuration dirty");
  item("rawread <0..3>", "Bound-only register read; bypasses driver health");
  item("rawwrite <1..3> <raw16>", "Bound-only write; bypasses health, marks dirty");
  item("rawdump", "Bound-only read of all registers; bypasses health");
  section("Diagnostics");
  item("drv / health / state / online", "Cached driver health and independent bus counters");
  item("diag", "Version, desired settings and cached health");
  item("probe", "Read device configuration without driver health effects");
  item("recover", "Cooperatively reapply desired configuration");
  item("busreset / reset all", "General-call RESET of ALL compatible bus devices");
  item("ara / alertresponse", "Acknowledge one SMBus ALERT response winner");
  item("stress [N]", "Finite cooperative samples, default 100; stop cancels");
  item("stress_mix [N]", "Finite probe/config/threshold/sample cycles; default 100");
  item("selfcheck / selftest", "Cooperative read-only PASS/FAIL/SKIP chip checks");
  item("selftest full", "Mutate/test all settings, then restore captured profile");
  item("xfer_stats / counters", "Independent cached adapter transfer counters");
  item("xfer_reset", "Reset adapter counters while idle; preserves health");
  item("healthreset", "Clear cumulative health totals while idle; retain state/error");
  print("\nRegister reads and ARA can acknowledge interrupt ALERT. No automatic bus reset.\n");
  print("Full-test stop/cancel starts baseline restoration; a second cancellation aborts restoration.\n");
}
void Cli::printSample(const TMP1x2::Sample& sample) {
  _lastSample = sample;
  _hasSample = true;
  print("Temperature: %s%.4f C%s raw=0x%04X counts=%d extended=%s timestamp=%lu ms clock=%s one-shot=%s\n",
        color(32), static_cast<double>(sample.celsius), color(0), static_cast<unsigned>(sample.raw),
        static_cast<int>(sample.counts), sample.extendedMode ? "yes" : "no",
        static_cast<unsigned long>(sample.timestampMs), sample.timestampValid ? "known" : "unknown",
        sample.freshConversion ? "confirmed" : "unproven");
}
void Cli::printSettings() {
  print("Desired: model=%s address=0x%02X timeout=%lu ms mode=%s rate=%s extended=%s\n",
        TMP1x2::toString(_config.model), _config.i2cAddress, static_cast<unsigned long>(_config.i2cTimeoutMs),
        _config.mode == TMP1x2::Mode::CONTINUOUS ? "continuous" : "shutdown",
        TMP1x2::toString(_config.conversionRate), _config.extendedMode ? "yes" : "no");
  print("Thresholds: low=%.4f C high=%.4f C alert=%s polarity=%s faults=%s offline-threshold=%u\n",
        static_cast<double>(_config.lowThresholdC), static_cast<double>(_config.highThresholdC),
        _config.alertMode == TMP1x2::AlertMode::COMPARATOR ? "comparator" : "interrupt",
        _config.alertPolarity == TMP1x2::AlertPolarity::ACTIVE_LOW ? "low" : "high",
        TMP1x2::toString(_config.faultQueue), _config.offlineThreshold);
  print("Address range: 0x%02X..0x%02X; physical ALERT=%s pin=%d\n",
        TMP1x2::modelAddressMin(_config.model), TMP1x2::modelAddressMax(_config.model),
        TMP1x2::hasAlertOutput(_config.model) ? "available" : "not present", _config.alertPin);
}
void Cli::printTiming() {
  print("Timing: conversion maximum=%lu ms continuous period=%lu ms (%s)\n",
        static_cast<unsigned long>(TMP1x2::cmd::CONVERSION_TIME_MAX_MS),
        static_cast<unsigned long>(TMP1x2::TMP1x2::conversionPeriodMs(_config.conversionRate)),
        TMP1x2::toString(_config.conversionRate));
  print("Resolution: 0.0625 C/count; normal 12-bit [-128, 127.9375] C; extended 13-bit [-256, 255.9375] C.\n");
  print("Register ranges do not expand sensor accuracy specifications. Continuous reads have no freshness marker.\n");
  print("EM changes settle the old format and complete a new-format conversion; each wait includes a 1 ms clock margin.\n");
}
void Cli::printTransferStats() {
  if (!_platform.transferStats) { print("No adapter transfer counters.\n"); return; }
  const auto bus = _platform.transferStats(_platform.user);
  print("Bus: attempts=%lu ok=%lu fail=%lu (includes raw/scan/probe; independent of driver health)\n",
        static_cast<unsigned long>(bus.attempts), static_cast<unsigned long>(bus.successes),
        static_cast<unsigned long>(bus.failures));
}
void Cli::printRunStats() {
  if (!_run.available) { print("No watch/stress run statistics.\n"); return; }
  const uint32_t elapsed = (_watch ? now() : _run.endedMs) - _run.startedMs;
  print("Run: %s target=%lu completed=%lu ok=%lu fail=%lu elapsed=%lu ms remaining=%lu\n",
        _watch ? "active" : "stopped", static_cast<unsigned long>(_run.target),
        static_cast<unsigned long>(_run.mixed ? _run.completedCycles : _run.successes + _run.failures),
        static_cast<unsigned long>(_run.successes), static_cast<unsigned long>(_run.failures),
        static_cast<unsigned long>(elapsed), static_cast<unsigned long>(_remaining));
  if (_run.mixed)
    print("Mixed checks: completed=%lu samples=%lu (four checks per complete cycle; shutdown sampling may trigger OS)\n",
          static_cast<unsigned long>(_run.successes + _run.failures), static_cast<unsigned long>(_run.samples));
  if (_run.hasSample)
    print("Temperature summary: min=%.4f C max=%.4f C mean=%.4f C\n",
          static_cast<double>(_run.minC), static_cast<double>(_run.maxC),
          _run.sumC / static_cast<double>(_run.samples));
  const uint32_t success = _watch ? _device.totalSuccess() : _run.healthSuccessAfter;
  const uint32_t failure = _watch ? _device.totalFailures() : _run.healthFailureAfter;
  print("Tracked health delta: ok=%lu fail=%lu\n",
        static_cast<unsigned long>(success - _run.healthSuccessBefore),
        static_cast<unsigned long>(failure - _run.healthFailureBefore));
  bool saturated = success == UINT32_MAX || failure == UINT32_MAX;
  if (_run.busAvailable) {
    const auto bus = _watch ? _platform.transferStats(_platform.user) : _run.busAfter;
    print("Adapter delta: attempts=%lu ok=%lu fail=%lu\n",
          static_cast<unsigned long>(bus.attempts - _run.busBefore.attempts),
          static_cast<unsigned long>(bus.successes - _run.busBefore.successes),
          static_cast<unsigned long>(bus.failures - _run.busBefore.failures));
    saturated = saturated || bus.attempts == UINT32_MAX || bus.successes == UINT32_MAX || bus.failures == UINT32_MAX;
  }
  if (saturated) print("Counter saturation reached; deltas are lower bounds.\n");
  if (_run.failures) {
    print("First error: "); status(_run.firstError);
    print("Last error: "); status(_run.lastError);
  }
}
void Cli::printOperation() {
  if (_diagnostic.active) {
    printDiagnostic();
    if (!_device.operationActive()) return;
  }
  const auto operation = _device.getOperationSnapshot();
  print("Operation: active=%s token=%lu kind=%s status=%s waiting=%s next=%lu ms timeout=%lu ms write-attempted=%s\n",
        operation.active ? "yes" : "no", static_cast<unsigned long>(operation.token), TMP1x2::toString(operation.kind),
        TMP1x2::errorName(operation.status.code), operation.waiting ? "yes" : "no",
        static_cast<unsigned long>(operation.nextPollMs), static_cast<unsigned long>(operation.timeoutMs),
        operation.hardwareEffectPossible ? "yes" : "no");
  if (operation.active && operation.kind != TMP1x2::OperationKind::READ)
    print("Staged: mode=%s rate=%s extended=%s low=%.4f C high=%.4f C\n",
          TMP1x2::toString(operation.desiredConfig.mode), TMP1x2::toString(operation.desiredConfig.conversionRate),
          operation.desiredConfig.extendedMode ? "yes" : "no", static_cast<double>(operation.desiredConfig.lowThresholdC),
          static_cast<double>(operation.desiredConfig.highThresholdC));
}
void Cli::printOperationResult() {
  if (_lastResultDiagnostic) { printDiagnosticResult(); return; }
  if (!_hasOperationResult) { print("No completed operation result.\n"); return; }
  print("Operation result: token=%lu kind=%s elapsed=%lu ms write-attempted=%s\n",
        static_cast<unsigned long>(_lastOperation.token), TMP1x2::toString(_lastOperation.kind),
        static_cast<unsigned long>(_lastOperation.completedMs - _lastOperation.startedMs),
        _lastOperation.hardwareEffectPossible ? "yes" : "no");
  status(_lastOperation.status);
  if (_lastOperation.hasSample)
    print("Result sample: %.4f C raw=0x%04X timestamp=%lu ms\n", static_cast<double>(_lastOperation.sample.celsius),
          static_cast<unsigned>(_lastOperation.sample.raw), static_cast<unsigned long>(_lastOperation.sample.timestampMs));
}
void Cli::printDiagnostic(bool last) {
  const auto& diagnostic = last ? _lastDiagnostic : _diagnostic;
  const char* name = diagnostic.kind == DiagnosticKind::SCAN ? "scan" :
      diagnostic.kind == DiagnosticKind::DISCOVER ? "discover" :
      diagnostic.kind == DiagnosticKind::SELFCHECK ? "selfcheck" :
      diagnostic.kind == DiagnosticKind::SETTINGS ? "settings read" :
      diagnostic.kind == DiagnosticKind::SNAPSHOT ? "snapshot read" : "selftest full";
  print("Diagnostic: %s active=%s phase=%u checked=%lu elapsed=%lu ms restoring=%s\n", name,
        diagnostic.active ? "yes" : "no", static_cast<unsigned>(diagnostic.phase),
        static_cast<unsigned long>(diagnostic.checked),
        static_cast<unsigned long>((diagnostic.active ? now() : diagnostic.endedMs) - diagnostic.startedMs),
        diagnostic.restoring ? "yes" : "no");
}
void Cli::printDiagnosticResult() {
  const auto& diagnostic = _lastDiagnostic;
  if (!diagnostic.available) { print("No diagnostic result.\n"); return; }
  printDiagnostic(true);
  if (diagnostic.kind == DiagnosticKind::SCAN || diagnostic.kind == DiagnosticKind::DISCOVER) {
    print("Scan %s: %lu probed, %lu ACK, %lu other transport errors.\n",
          diagnostic.cancelled ? "cancelled" : diagnostic.active ? "active" : "complete",
          static_cast<unsigned long>(diagnostic.checked), static_cast<unsigned long>(diagnostic.found),
          static_cast<unsigned long>(diagnostic.failures));
  } else {
    print("Diagnostic summary: %s pass=%lu fail=%lu skip=%lu\n",
          diagnostic.cancelled ? "cancelled" : diagnostic.active ? "active" : "complete",
          static_cast<unsigned long>(diagnostic.passed), static_cast<unsigned long>(diagnostic.failures),
          static_cast<unsigned long>(diagnostic.skipped));
    if (diagnostic.kind == DiagnosticKind::CONFIGTEST) {
      print("Baseline restoration: %s\n", diagnostic.restoreComplete ? "verified" : "not verified; run begin to retry the captured profile");
      if (!diagnostic.restoreStatus.ok()) status(diagnostic.restoreStatus);
    }
  }
  if (diagnostic.failures) { print("Last diagnostic failure: "); status(diagnostic.lastError); }
}
void Cli::printHealth() {
  const auto state = _device.state();
  unsigned shade = state == TMP1x2::DriverState::READY ? 32U :
                   state == TMP1x2::DriverState::OFFLINE ? 31U : 33U;
  print("Health: state=%s%s%s online=%s consec=%u ok=%lu fail=%lu\n", color(shade),
        TMP1x2::driverStateName(state), color(0), _device.isOnline() ? "yes" : "no",
        static_cast<unsigned>(_device.consecutiveFailures()),
        static_cast<unsigned long>(_device.totalSuccess()), static_cast<unsigned long>(_device.totalFailures()));
  const uint64_t attempts = static_cast<uint64_t>(_device.totalSuccess()) + _device.totalFailures();
  if (attempts == 0) print("  Success rate: %sn/a%s\n", color(90), color(0));
  else {
    const double rate = 100.0 * static_cast<double>(_device.totalSuccess()) / static_cast<double>(attempts);
    print("  Success rate: %s%.1f%%%s\n", color(rate >= 99.9 ? 32U : rate >= 80.0 ? 33U : 31U), rate, color(0));
  }
  const auto error = _device.lastError();
  print("Trust: bound=%s initialized=%s dirty=%s%s%s last-ok=%lu ms last-error=%lu ms %s detail=%ld\n",
        _device.isBound() ? "yes" : "no", _device.isInitialized() ? "yes" : "no",
        color(_device.hardwareConfigDirty() ? 33U : 32U), _device.hardwareConfigDirty() ? "yes" : "no", color(0),
        static_cast<unsigned long>(_device.lastOkMs()), static_cast<unsigned long>(_device.lastErrorMs()),
        TMP1x2::errorName(error.code), static_cast<long>(error.detail));
  if (_device.hardwareConfigDirty()) {
    const auto reason = _device.hardwareConfigDirtyError();
    print("  Dirty reason: %s detail=%ld%s%s\n", TMP1x2::errorName(reason.code),
          static_cast<long>(reason.detail), reason.msg && *reason.msg ? ": " : "",
          reason.msg ? reason.msg : "");
  }
  print("Conversion: pending=%s ready=%s\n", _device.conversionStarted() ? "yes" : "no",
        _device.conversionReady() ? "yes" : "no");
  if (_platform.transferStats) printTransferStats();
}
}  // namespace tmp1x2_cli
