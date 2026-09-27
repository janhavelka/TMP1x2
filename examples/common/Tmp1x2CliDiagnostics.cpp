#include "Tmp1x2Cli.h"

// Cooperative owner operations, finite diagnostics and sampling state machines.

namespace tmp1x2_cli {
namespace {
const char* configCaseName(uint8_t phase) {
  static const char* names[] = {"rate 0.25 Hz", "rate 1 Hz", "rate 4 Hz", "rate 8 Hz",
    "continuous mode", "shutdown mode", "normal format", "extended format",
    "comparator mode", "interrupt mode", "active-low polarity", "active-high polarity",
    "fault queue 1", "fault queue 2", "fault queue 4", "fault queue 6",
    "threshold window 0..50 C", "threshold window -10..30 C"};
  return phase < 18 ? names[phase] : "baseline restoration";
}
}  // namespace

void Cli::recordRunResult(TMP1x2::Status result, const TMP1x2::Sample& sample, bool hasSample) {
  if (result.ok()) {
    ++_run.successes;
    if (hasSample) {
      if (!_run.hasSample) { _run.minC = sample.celsius; _run.maxC = sample.celsius; _run.hasSample = true; }
      if (sample.celsius < _run.minC) _run.minC = sample.celsius;
      if (sample.celsius > _run.maxC) _run.maxC = sample.celsius;
      _run.sumC += static_cast<double>(sample.celsius);
      ++_run.samples;
      _lastSample = sample; _hasSample = true;
      if (_verbose) printSample(sample);
    } else if (_verbose) status(result);
  } else {
    if (_run.failures == 0) _run.firstError = result;
    _run.lastError = result;
    ++_run.failures;
    if (_verbose) status(result);
  }
}
TMP1x2::Status Cli::startOperation(TMP1x2::OperationKind kind, const TMP1x2::Config* desired) {
  if (!_platform.nowMs && !_config.nowMs) {
    const auto st = TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "cooperative operations require a monotonic clock");
    status(st); return st;
  }
  TMP1x2::Status st{};
  if (kind == TMP1x2::OperationKind::INITIALIZE) {
    st = _device.bind(_config);
    if (!st.ok()) { status(st); return st; }
    _config = _device.getConfig();
    st = _device.startInitialize(now(), 1000, _operationToken);
  } else if (kind == TMP1x2::OperationKind::CONFIGURE && desired) {
    st = _device.startConfigure(*desired, now(), 1000, _operationToken);
  } else if (kind == TMP1x2::OperationKind::RECOVER) {
    st = _device.startRecover(now(), 1000, _operationToken);
  } else if (kind == TMP1x2::OperationKind::SHUTDOWN) {
    st = _device.startShutdown(now(), 1000, _operationToken);
  } else if (kind == TMP1x2::OperationKind::READ) {
    st = _device.startRead(now(), 1000, _operationToken);
  } else st = TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "unknown operation");
  if (st.inProgress()) {
    if (kind != TMP1x2::OperationKind::READ) _hasSample = false;
    print("Operation started: token=%lu kind=%s; job and cancel remain available.\n",
          static_cast<unsigned long>(_operationToken), TMP1x2::toString(kind));
  }
  status(st);
  return st;
}
void Cli::finishOperation() {
  if (!_device.resultPending()) return;
  TMP1x2::OperationResult result{};
  const auto st = _device.takeResult(_operationToken, result);
  if (!st.ok()) { status(st); return; }
  _lastOperation = result; _hasOperationResult = true;
  if (_device.isBound()) _config = _device.getConfig();
  _oneShot = _device.conversionStarted();
  if (result.hasSample) { _lastSample = result.sample; _hasSample = true; }
  if (_diagnostic.active && _diagnostic.kind == DiagnosticKind::CONFIGTEST) {
    finishConfigTestOperation(result); return;
  }
  _lastResultDiagnostic = false;
  printOperationResult();
  printPrompt();
}
bool Cli::activeWork() const { return _watch || _diagnostic.active || _device.operationActive(); }
void Cli::startDiagnostic(DiagnosticKind kind) {
  if (activeWork()) { print("Stop active work before starting another diagnostic.\n"); return; }
  if ((kind == DiagnosticKind::SCAN || kind == DiagnosticKind::DISCOVER) && !_platform.probeAddress) {
    print("No scan adapter.\n"); return;
  }
  if (kind == DiagnosticKind::CONFIGTEST && !_device.isInitialized()) {
    status(TMP1x2::Status::Error(TMP1x2::Err::NOT_INITIALIZED, "Initialize before the full configuration test")); return;
  }
  if (kind == DiagnosticKind::CONFIGTEST && _device.conversionStarted()) {
    status(TMP1x2::Status::Error(TMP1x2::Err::BUSY, "Consume the pending one-shot before the full configuration test")); return;
  }
  _diagnostic = Diagnostic{};
  _diagnostic.kind = kind; _diagnostic.active = true; _diagnostic.available = true;
  _diagnostic.startedMs = now(); _diagnostic.baseline = _config;
  _diagnostic.nextAddress = kind == DiagnosticKind::SCAN ? 0x08 : 0x40;
  if (kind == DiagnosticKind::CONFIGTEST) {
    _hasSample = false;
    print("Full configuration test: 18 verified cases; CHANGES mode, rate, EM, thermostat, polarity, fault queue and thresholds.\n");
    print("Cases use representable threshold windows; final restoration reinstates the captured desired profile.\n");
    print("First stop/cancel requests restoration; a second stops restoration and leaves reinitialization required.\n");
  } else if (kind == DiagnosticKind::SELFCHECK) {
    print("Selfcheck: read-only binding/configuration/threshold/temperature/GPIO checks; reads can acknowledge interrupt ALERT.\n");
  } else if (kind == DiagnosticKind::SETTINGS || kind == DiagnosticKind::SNAPSHOT) {
    print("Reading live configuration and thresholds cooperatively; no configuration writes.\n");
  } else print("Cooperative %s started; one address per tick, stop cancels. ACK does not identify a chip.\n",
               kind == DiagnosticKind::SCAN ? "scan" : "discovery");
}
void Cli::diagnosticCheck(const char* label, TMP1x2::Status result, const char* skip) {
  ++_diagnostic.checked;
  if (skip) {
    ++_diagnostic.skipped;
    print("  %s[SKIP]%s %s: %s\n", color(33), color(0), label, skip);
  } else if (result.ok()) {
    ++_diagnostic.passed;
    print("  %s[PASS]%s %s\n", color(32), color(0), label);
  } else {
    ++_diagnostic.failures; _diagnostic.lastError = result;
    print("  %s[FAIL]%s %s: %s detail=%ld%s%s\n", color(31), color(0), label,
          TMP1x2::errorName(result.code), static_cast<long>(result.detail),
          result.msg && *result.msg ? ": " : "", result.msg ? result.msg : "");
  }
}
void Cli::finishDiagnostic(bool cancelled) {
  _diagnostic.active = false; _diagnostic.cancelled = _diagnostic.cancelled || cancelled;
  _diagnostic.endedMs = now(); _lastResultDiagnostic = true;
  _lastDiagnostic = _diagnostic;
  printDiagnosticResult(); printPrompt();
}
void Cli::restoreProfile() {
  _diagnostic.restoring = true;
  const auto st = startOperation(TMP1x2::OperationKind::CONFIGURE, &_diagnostic.baseline);
  if (!st.inProgress()) {
    TMP1x2::OperationResult result{}; result.status = st;
    finishConfigTestOperation(result);
  }
}
void Cli::finishConfigTestOperation(const TMP1x2::OperationResult& result) {
  if (_diagnostic.restoring) {
    _diagnostic.restoreStatus = result.status; _diagnostic.restoreComplete = result.status.ok();
    diagnosticCheck("restore captured profile", result.status);
    if (!result.status.ok()) {
      // A failure before the first restore write leaves the preceding test case
      // cached by the core. Rebind locally so explicit recovery uses baseline.
      _config = _diagnostic.baseline;
      const auto binding = _device.bind(_config);
      if (!binding.ok()) status(binding);
      else {
        status(_device.invalidateDeviceState());
        print("Captured profile rebound without I2C; dirty evidence retained, driver health starts a new session.\n");
      }
    }
    finishDiagnostic(); return;
  }
  diagnosticCheck(configCaseName(_diagnostic.phase), result.status);
  if (!result.status.ok()) { restoreProfile(); return; }
  ++_diagnostic.phase;
}
void Cli::cancelWork() {
  if (_diagnostic.active) {
    if (_diagnostic.kind != DiagnosticKind::CONFIGTEST) { finishDiagnostic(true); return; }
    _diagnostic.cancelled = true;
    const bool stoppingRestore = _diagnostic.restoring;
    if (_device.operationActive()) {
      (void)_device.cancel();
      TMP1x2::OperationResult result{};
      if (_device.takeResult(_operationToken, result).ok()) {
        _lastOperation = result; _hasOperationResult = true;
      }
    }
    if (stoppingRestore) {
      _diagnostic.restoreStatus = TMP1x2::Status::Error(TMP1x2::Err::CANCELLED, "Restoration cancelled by user");
      _config = _diagnostic.baseline; const auto st = _device.bind(_config); status(st);
      if (st.ok()) {
        status(_device.invalidateDeviceState());
        print("Restoration stopped; baseline rebound without I2C, dirty evidence retained, health starts a new session.\n");
      }
      finishDiagnostic(true);
    } else {
      print("Configuration test cancelled; scheduling restoration of the captured profile.\n");
      restoreProfile();
    }
    return;
  }
  if (_device.operationActive()) { status(_device.cancel()); finishOperation(); return; }
  stop();
}
void Cli::tickDiagnostic() {
  if (_diagnostic.kind == DiagnosticKind::SCAN || _diagnostic.kind == DiagnosticKind::DISCOVER) {
    const uint8_t address = _diagnostic.nextAddress;
    const auto st = _platform.probeAddress(address, _platform.user);
    ++_diagnostic.checked;
    if (st.ok()) {
      ++_diagnostic.found;
      print("  Found 0x%02X%s\n", address,
            address >= 0x40 && address <= 0x43 ? " (TMP112D ADD0-compatible; identity unverified)" :
            address >= 0x48 && address <= 0x4B ? " (TMP102/TMP112-compatible; identity unverified)" : "");
    } else if (!st.is(TMP1x2::Err::I2C_NACK_ADDR) && !st.is(TMP1x2::Err::DEVICE_NOT_FOUND)) {
      ++_diagnostic.failures; _diagnostic.lastError = st;
    }
    ++_diagnostic.nextAddress;
    if (_diagnostic.kind == DiagnosticKind::DISCOVER && _diagnostic.nextAddress == 0x44) _diagnostic.nextAddress = 0x48;
    if (_diagnostic.nextAddress > (_diagnostic.kind == DiagnosticKind::SCAN ? 0x77 : 0x4B)) finishDiagnostic();
    return;
  }
  if (_diagnostic.kind == DiagnosticKind::CONFIGTEST) {
    if (_diagnostic.phase == 18) { restoreProfile(); return; }
    auto desired = _diagnostic.baseline;
    // Every case remains representable when the baseline uses extended-only thresholds.
    desired.lowThresholdC = 20; desired.highThresholdC = 30;
    const unsigned phase = _diagnostic.phase;
    if (phase < 4) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(phase);
    else if (phase < 6) desired.mode = static_cast<TMP1x2::Mode>(phase - 4);
    else if (phase < 8) desired.extendedMode = phase == 7;
    else if (phase < 10) desired.alertMode = static_cast<TMP1x2::AlertMode>(phase - 8);
    else if (phase < 12) desired.alertPolarity = static_cast<TMP1x2::AlertPolarity>(phase - 10);
    else if (phase < 16) desired.faultQueue = static_cast<TMP1x2::FaultQueue>(phase - 12);
    else if (phase == 16) { desired.lowThresholdC = 0; desired.highThresholdC = 50; }
    else { desired.lowThresholdC = -10; desired.highThresholdC = 30; }
    const auto st = startOperation(TMP1x2::OperationKind::CONFIGURE, &desired);
    if (!st.inProgress()) {
      TMP1x2::OperationResult result{}; result.status = st; finishConfigTestOperation(result);
    }
    return;
  }
  const bool settings = _diagnostic.kind == DiagnosticKind::SETTINGS;
  const uint8_t phase = _diagnostic.kind == DiagnosticKind::SNAPSHOT ? 8 :
      settings ? static_cast<uint8_t>(_diagnostic.phase + 3) : _diagnostic.phase;
  TMP1x2::Status st{};
  if (phase == 0) {
    diagnosticCheck("transport binding", _device.isBound() ? st : TMP1x2::Status::Error(TMP1x2::Err::NOT_BOUND, "No transport binding"));
  } else if (phase == 1) {
    diagnosticCheck("initialized driver", _device.isInitialized() ? st : TMP1x2::Status::Error(TMP1x2::Err::NOT_INITIALIZED, "Run begin first"));
  } else if (phase == 2) {
    if (!_device.isBound()) diagnosticCheck("presence/configuration plausibility", st, "transport unbound");
    else diagnosticCheck("presence/configuration plausibility", _device.probe());
  } else if (!_device.isInitialized()) {
    diagnosticCheck(phase == 3 ? "live configuration" : phase == 4 ? "desired readback verification" :
                    phase == 5 ? "thresholds" : phase == 6 ? "temperature register" :
                    phase == 7 ? "physical ALERT GPIO" : "coherent register snapshot", st, "driver uninitialized");
  } else if (phase == 3) {
    TMP1x2::ConfigurationInfo info{}; st = _device.readConfiguration(info);
    diagnosticCheck("live configuration fixed bits", st);
    if (st.ok()) print("  CONFIG=0x%04X mode=%s rate=%s EM=%u thermostat=%s polarity=%s faults=%s AL=%u active=%u\n",
                       static_cast<unsigned>(info.raw), TMP1x2::toString(info.mode), TMP1x2::toString(info.conversionRate),
                       info.extendedMode ? 1U : 0U, TMP1x2::toString(info.alertMode), TMP1x2::toString(info.alertPolarity),
                       TMP1x2::toString(info.faultQueue), info.alert ? 1U : 0U, info.alertActive ? 1U : 0U);
  } else if (phase == 4) {
    diagnosticCheck("desired configuration and threshold readback", _device.verifyConfiguration());
  } else if (phase == 5) {
    float low = 0, high = 0; st = _device.readThresholds(low, high);
    if (st.ok() && low > high) st = TMP1x2::Status::Error(TMP1x2::Err::CONFIG_MISMATCH, "Threshold ordering invalid");
    diagnosticCheck("decoded thresholds and ordering", st);
    if (st.ok()) print("  Hardware thresholds: low=%.4f C high=%.4f C\n", static_cast<double>(low), static_cast<double>(high));
  } else if (phase == 6) {
    TMP1x2::Sample sample{}; st = _device.readSample(sample);
    diagnosticCheck("temperature register format", st);
    if (st.ok()) { printSample(sample); print("  Latest-register check does not prove a fresh conversion, especially in shutdown.\n"); }
  } else if (phase == 7) {
    if (!TMP1x2::hasAlertOutput(_config.model)) diagnosticCheck("physical ALERT GPIO", st, "model has ADD0 instead of ALERT");
    else if (!_config.gpioRead || _config.alertPin < 0) diagnosticCheck("physical ALERT GPIO", st, "optional GPIO not configured");
    else {
      bool active = false; st = _device.readAlertPin(active); diagnosticCheck("physical ALERT GPIO", st);
      if (st.ok()) print("  ALERT=%s pin=%d; no I2C\n", active ? "active" : "inactive", _config.alertPin);
    }
  } else {
    TMP1x2::RegisterSnapshot snapshot{}; st = _device.readSnapshot(snapshot);
    diagnosticCheck("coherent register snapshot", st);
    if (st.ok())
      print("  Snapshot: CONFIG=0x%04X TEMP=0x%04X TLOW=0x%04X THIGH=0x%04X config-match=%s thresholds-match=%s temperature-trusted=%s\n",
            static_cast<unsigned>(snapshot.configuration.raw), static_cast<unsigned>(snapshot.rawTemperature),
            static_cast<unsigned>(snapshot.rawLowThreshold), static_cast<unsigned>(snapshot.rawHighThreshold),
            snapshot.configurationMatchesDesired ? "yes" : "no", snapshot.thresholdsMatchDesired ? "yes" : "no",
            snapshot.temperatureTrusted ? "yes" : "no");
  }
  ++_diagnostic.phase;
  if (_diagnostic.phase == (_diagnostic.kind == DiagnosticKind::SNAPSHOT ? 1 : settings ? 3 : 9)) finishDiagnostic();
}
void Cli::stop() {
  if (_watch) {
    _run.endedMs = now();
    _run.healthSuccessAfter = _device.totalSuccess(); _run.healthFailureAfter = _device.totalFailures();
    if (_run.busAvailable) _run.busAfter = _platform.transferStats(_platform.user);
  }
  _watch = false;
  _remaining = 0;
  print("%s stopped: ok=%lu fail=%lu\n", _run.mixed ? "Mixed stress" : "Watch", static_cast<unsigned long>(_run.successes),
        static_cast<unsigned long>(_run.failures));
  printRunStats();
  if (_device.conversionStarted())
    print("Pending one-shot retained; tryread or another watch can consume it.\n");
}
void Cli::tickMixed(uint32_t nowMs) {
  TMP1x2::Status st{};
  TMP1x2::Sample sample{};
  const char* label = nullptr;
  if (_mixedPhase == 0) {
    label = "probe"; st = _device.probe();
  } else if (_mixedPhase == 1) {
    label = "configuration";
    TMP1x2::ConfigurationInfo info{}; st = _device.readConfiguration(info);
  } else if (_mixedPhase == 2) {
    label = "thresholds";
    float low = 0, high = 0; st = _device.readThresholds(low, high);
  } else if (_config.mode == TMP1x2::Mode::SHUTDOWN) {
    if (!_oneShot) {
      st = _device.startOneShot();
      if (st.ok()) { _oneShot = true; _conversionDeadlineMs = nowMs + 500; return; }
    } else {
      st = _device.tryRead(sample);
      if (st.is(TMP1x2::Err::MEASUREMENT_NOT_READY) || st.inProgress()) {
        if (static_cast<int32_t>(nowMs - _conversionDeadlineMs) < 0) return;
        recordRunResult(TMP1x2::Status::Error(TMP1x2::Err::TIMEOUT,
                        "mixed sample deadline; pending one-shot retained"), sample);
        ++_run.completedCycles;
        stop(); printPrompt(); return;
      }
      _oneShot = _device.conversionStarted();
    }
  } else st = _device.readSample(sample);
  // Observer APIs return readable hardware values while separately latching
  // trust mismatches. A diagnostic check must report that mismatch explicitly.
  if ((_mixedPhase == 1 || _mixedPhase == 2) && st.ok() && _device.hardwareConfigDirty()) {
    st = _device.hardwareConfigDirtyError();
    if (st.ok()) st = TMP1x2::Status::Error(TMP1x2::Err::CONFIG_MISMATCH, "configuration remains dirty");
  }
  if (_verbose && label) print("Mixed %s: ", label);
  recordRunResult(st, sample, _mixedPhase == 3);
  if (++_mixedPhase == 4) {
    _mixedPhase = 0; ++_run.completedCycles;
    _nextMs = nowMs + _intervalMs;
    if (--_remaining == 0) { stop(); printPrompt(); }
  }
}
void Cli::tick() {
  const uint32_t nowMs = now();
  if (_device.operationActive()) {
    // One physical transfer per loop; conversion settling uses timestamps and
    // leaves command input, cached diagnostics, and cancellation responsive.
    (void)_device.poll(nowMs, 1);
    if (_device.isBound()) _config = _device.getConfig();
    finishOperation();
    return;
  }
  if (_platform.nowMs) (void)_device.poll(nowMs, 0);
  if (_diagnostic.active) { tickDiagnostic(); return; }
  // Only an active watch drives transfers automatically. A stopped/timed-out
  // watch or a manual one-shot stays pending until an explicit command joins it.
  if (!_watch || static_cast<int32_t>(nowMs - _nextMs) < 0) return;
  if (_run.mixed) { tickMixed(nowMs); return; }
  TMP1x2::Sample sample{};
  TMP1x2::Status st{};
  if (_config.mode == TMP1x2::Mode::SHUTDOWN) {
    if (!_oneShot) {
      st = _device.startOneShot();
      if (st.ok() || st.inProgress()) { _oneShot = true; _conversionDeadlineMs = nowMs + 500; return; }
    } else {
      st = _device.tryRead(sample);
      if (st.is(TMP1x2::Err::MEASUREMENT_NOT_READY) || st.inProgress()) {
        if (static_cast<int32_t>(nowMs - _conversionDeadlineMs) < 0) return;
        recordRunResult(TMP1x2::Status::Error(TMP1x2::Err::TIMEOUT, "watch conversion deadline; pending one-shot retained"), sample);
        stop(); printPrompt(); return;
      }
      // A transport failure need not consume the core's pending conversion.
      // Join it on the next attempt instead of issuing a new start that is BUSY.
      _oneShot = _device.conversionStarted();
    }
  } else st = _device.readSample(sample);
  recordRunResult(st, sample);
  _nextMs = nowMs + _intervalMs;
  if (--_remaining == 0) { stop(); printPrompt(); }
}
}  // namespace tmp1x2_cli
