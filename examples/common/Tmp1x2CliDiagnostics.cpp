#include "Tmp1x2Cli.h"

// Cooperative owner operations, finite diagnostics and sampling state machines.

namespace tmp1x2_cli {
namespace {
TMP1x2::Status observedStatus(const TMP1x2::TMP1x2& device, TMP1x2::Status result) {
  // Successful diagnostic transport can still expose an untrusted profile.
  if (!result.ok() || !device.hardwareConfigDirty()) return result;
  const auto dirty = device.hardwareConfigDirtyError();
  return dirty.ok() ? TMP1x2::Status::Error(TMP1x2::Err::CONFIG_MISMATCH, "configuration remains dirty") : dirty;
}
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
TMP1x2::Status Cli::startOperation(TMP1x2::OperationKind kind, const TMP1x2::Config* desired, uint32_t timeoutMs) {
  if (!_platform.nowMs && !_config.nowMs) {
    const auto st = TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "cooperative operations require a monotonic clock");
    status(st); return st;
  }
  TMP1x2::Status st{};
  if (kind == TMP1x2::OperationKind::INITIALIZE) {
    st = _device.bind(_config);
    if (!st.ok()) { status(st); return st; }
    _config = _device.getConfig();
    st = _device.startInitialize(now(), timeoutMs, _operationToken);
  } else if (kind == TMP1x2::OperationKind::CONFIGURE && desired) {
    st = _device.startConfigure(*desired, now(), timeoutMs, _operationToken);
  } else if (kind == TMP1x2::OperationKind::RECOVER) {
    st = _device.startRecover(now(), timeoutMs, _operationToken);
  } else if (kind == TMP1x2::OperationKind::SHUTDOWN) {
    st = _device.startShutdown(now(), timeoutMs, _operationToken);
  } else if (kind == TMP1x2::OperationKind::READ) {
    st = _device.startRead(now(), timeoutMs, _operationToken);
  } else st = TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "unknown operation");
  if (st.inProgress()) {
    if (kind != TMP1x2::OperationKind::READ) _hasSample = false;
    print("Operation started: token=%lu kind=%s; job and cancel remain available.\n",
          static_cast<unsigned long>(_operationToken), TMP1x2::toString(kind));
  }
  status(st);
  return st;
}
void Cli::pollOperation(uint8_t budget, bool report) {
  const auto result = _device.poll(now(), budget);
  if (_device.isBound()) _config = _device.getConfig();
  if (report) {
    print("Poll: budget=%u transfers=%u done=%s next=%lu ms\n",
          static_cast<unsigned>(budget), static_cast<unsigned>(result.transfers),
          result.done ? "yes" : "no", static_cast<unsigned long>(result.nextPollMs));
    status(result.status);
  }
  finishOperation();
}
void Cli::finishOperation() {
  if (!_device.resultPending()) return;
  TMP1x2::OperationResult result{};
  const auto st = _device.takeResult(_operationToken, result);
  if (!st.ok()) { status(st); return; }
  _lastOperation = result; _hasOperationResult = true;
  if (_device.isBound()) _config = _device.getConfig();
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
  if (activeWork()) { status(TMP1x2::Status::Error(TMP1x2::Err::BUSY, "Stop active work before starting another diagnostic.")); return; }
  if ((kind == DiagnosticKind::SCAN || kind == DiagnosticKind::DISCOVER) && !_platform.probeAddress) {
    status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "No scan adapter.")); return;
  }
  if (kind == DiagnosticKind::DISCOVER && !_config.i2cWriteRead) {
    status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "No register-read adapter for discovery.")); return;
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
  } else if (kind == DiagnosticKind::DISCOVER) {
    print("Cooperative discovery started; probe then CONFIG read at 0x40..0x43 and 0x48..0x4B, at most one callback per tick.\n");
    print("Fixed-bit plausibility is not chip identity; model/address stay selected by the user. Reads can acknowledge interrupt ALERT.\n");
  } else print("Cooperative scan started; one address probe per tick, stop cancels. ACK does not identify a chip.\n");
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
    TMP1x2::Status st{};
    uint16_t raw = 0;
    if (_diagnostic.kind == DiagnosticKind::DISCOVER && _diagnostic.phase == 1) {
      const uint8_t reg = TMP1x2::cmd::REG_CONFIG;
      uint8_t bytes[2]{};
      st = _config.i2cWriteRead(address, &reg, 1, bytes, sizeof(bytes), _config.i2cTimeoutMs, _config.i2cUser);
      raw = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8U) | bytes[1]);
    } else st = _platform.probeAddress(address, _platform.user);
    if (st.inProgress()) st = TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "Diagnostic callback must return a terminal status");
    if (_diagnostic.kind == DiagnosticKind::DISCOVER && _diagnostic.phase == 0 && st.ok()) {
      ++_diagnostic.found;
      _diagnostic.phase = 1;
      return; // The qualified register read has its own callback budget/tick.
    }
    ++_diagnostic.checked;
    if (st.ok()) {
      if (_diagnostic.kind == DiagnosticKind::DISCOVER) {
        const bool candidate = TMP1x2::TMP1x2::decodeConfiguration(raw).valid;
        if (candidate) ++_diagnostic.passed;
        else ++_diagnostic.incompatible;
        print("Discover address=0x%02X config=0x%04X candidate=%s code=%s detail=%ld identity=unverified\n",
              address, static_cast<unsigned>(raw), candidate ? "yes" : "no",
              candidate ? "OK" : "CONFIG_MISMATCH", candidate ? 0L : static_cast<long>(raw));
      } else { ++_diagnostic.found; print("  ACK 0x%02X; identity unverified\n", address); }
    } else if ((_diagnostic.kind == DiagnosticKind::SCAN || _diagnostic.phase == 0) &&
               (st.is(TMP1x2::Err::I2C_NACK_ADDR) || st.is(TMP1x2::Err::DEVICE_NOT_FOUND))) {
      ++_diagnostic.skipped;
    } else {
      ++_diagnostic.failures; _diagnostic.lastError = st;
    }
    if (!st.ok()) {
      if (_diagnostic.kind == DiagnosticKind::DISCOVER)
        print("Discover address=0x%02X config=unavailable candidate=no code=%s detail=%ld identity=unverified\n",
              address, TMP1x2::errorName(st.code), static_cast<long>(st.detail));
      else if (!st.is(TMP1x2::Err::I2C_NACK_ADDR) && !st.is(TMP1x2::Err::DEVICE_NOT_FOUND))
        print("  Scan error address=0x%02X code=%s detail=%ld%s%s\n", address,
              TMP1x2::errorName(st.code), static_cast<long>(st.detail),
              st.msg && *st.msg ? ": " : "", st.msg ? st.msg : "");
    }
    _diagnostic.phase = 0;
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
    if (st.ok()) print("  CONFIG=0x%04X mode=%s rate=%s EM=%u thermostat=%s polarity=%s faults=%s AL=%u active=%u\n",
                       static_cast<unsigned>(info.raw), TMP1x2::toString(info.mode), TMP1x2::toString(info.conversionRate),
                       info.extendedMode ? 1U : 0U, TMP1x2::toString(info.alertMode), TMP1x2::toString(info.alertPolarity),
                       TMP1x2::toString(info.faultQueue), info.alert ? 1U : 0U, info.alertActive ? 1U : 0U);
    diagnosticCheck("live configuration and trust", observedStatus(_device, st));
  } else if (phase == 4) {
    diagnosticCheck("desired configuration and threshold readback", _device.verifyConfiguration());
  } else if (phase == 5) {
    float low = 0, high = 0; st = _device.readThresholds(low, high);
    if (st.ok()) print("  Hardware thresholds: low=%.4f C high=%.4f C\n", static_cast<double>(low), static_cast<double>(high));
    if (st.ok() && low > high) st = TMP1x2::Status::Error(TMP1x2::Err::CONFIG_MISMATCH, "Threshold ordering invalid");
    diagnosticCheck("decoded thresholds and ordering", observedStatus(_device, st));
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
    if (st.ok()) {
      print("  Snapshot: CONFIG=0x%04X TEMP=0x%04X TLOW=0x%04X THIGH=0x%04X config-match=%s thresholds-match=%s temperature-trusted=%s\n",
            static_cast<unsigned>(snapshot.configuration.raw), static_cast<unsigned>(snapshot.rawTemperature),
            static_cast<unsigned>(snapshot.rawLowThreshold), static_cast<unsigned>(snapshot.rawHighThreshold),
            snapshot.configurationMatchesDesired ? "yes" : "no", snapshot.thresholdsMatchDesired ? "yes" : "no",
            snapshot.temperatureTrusted ? "yes" : "no");
      if (!snapshot.configurationMatchesDesired || !snapshot.thresholdsMatchDesired || !snapshot.temperatureTrusted)
        st = TMP1x2::Status::Error(TMP1x2::Err::CONFIG_MISMATCH, "Snapshot differs from the desired profile or temperature is untrusted");
    }
    diagnosticCheck("coherent register snapshot", observedStatus(_device, st));
  }
  ++_diagnostic.phase;
  if (_diagnostic.phase == (_diagnostic.kind == DiagnosticKind::SNAPSHOT ? 1 : settings ? 3 : 9)) finishDiagnostic();
}
void Cli::stop() {
  if (_watch) {
    _run.elapsedMs += static_cast<uint32_t>(now() - _run.lastTickMs);
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
bool Cli::tickRunSample(uint32_t nowMs) {
  TMP1x2::Status st{};
  TMP1x2::Sample sample{};
  if (_config.mode == TMP1x2::Mode::SHUTDOWN) {
    // The driver owns pending-conversion state, including after transport errors.
    if (!_device.conversionStarted()) {
      st = _device.startOneShot();
      if (st.ok()) { _conversionDeadlineMs = nowMs + 500; return false; }
    } else {
      st = _device.tryRead(sample);
      if (st.is(TMP1x2::Err::MEASUREMENT_NOT_READY) || st.inProgress()) {
        if (static_cast<int32_t>(nowMs - _conversionDeadlineMs) < 0) return false;
        recordRunResult(TMP1x2::Status::Error(TMP1x2::Err::TIMEOUT,
                        _run.mixed ? "mixed sample deadline; pending one-shot retained" :
                                     "watch conversion deadline; pending one-shot retained"), sample);
        if (_run.mixed) ++_run.completedCycles;
        stop(); printPrompt(); return false;
      }
    }
  } else st = _device.readSample(sample);
  recordRunResult(st, sample);
  return true;
}
void Cli::tickMixed(uint32_t nowMs) {
  if (_mixedPhase == 3) {
    if (!tickRunSample(nowMs)) return;
  } else {
    TMP1x2::Status st{};
    const char* label = nullptr;
    if (_mixedPhase == 0) {
      label = "probe"; st = _device.probe();
    } else if (_mixedPhase == 1) {
      label = "configuration";
      TMP1x2::ConfigurationInfo info{}; st = _device.readConfiguration(info);
    } else {
      label = "thresholds";
      float low = 0, high = 0; st = _device.readThresholds(low, high);
    }
    // Observer APIs return readable hardware values while separately latching
    // trust mismatches. A diagnostic check must report that mismatch explicitly.
    if (_mixedPhase != 0) st = observedStatus(_device, st);
    if (_verbose) print("Mixed %s: ", label);
    recordRunResult(st, {}, false);
  }
  if (++_mixedPhase == 4) {
    _mixedPhase = 0; ++_run.completedCycles;
    _nextMs = nowMs + _intervalMs;
    if (--_remaining == 0) { stop(); printPrompt(); }
  }
}
void Cli::tick() {
  const uint32_t nowMs = now();
  if (_watch) {
    // Accumulate between ticks so long watches survive the 32-bit clock wrap.
    _run.elapsedMs += static_cast<uint32_t>(nowMs - _run.lastTickMs);
    _run.lastTickMs = nowMs;
  }
  if (_device.operationActive()) {
    // One physical transfer per loop; conversion settling uses timestamps and
    // leaves command input, cached diagnostics, and cancellation responsive.
    // Explicit diagnostics own their scheduling, including full-test restore.
    // Manual mode pauses only owner jobs admitted directly through the CLI.
    if (_jobAuto || _diagnostic.active) pollOperation(1);
    return;
  }
  if (_platform.nowMs) (void)_device.poll(nowMs, 0);
  if (_diagnostic.active) { tickDiagnostic(); return; }
  // Only an active watch drives transfers automatically. A stopped/timed-out
  // watch or a manual one-shot stays pending until an explicit command joins it.
  if (!_watch || static_cast<int32_t>(nowMs - _nextMs) < 0) return;
  if (_run.mixed) { tickMixed(nowMs); return; }
  if (!tickRunSample(nowMs)) return;
  _nextMs = nowMs + _intervalMs;
  if (--_remaining == 0) { stop(); printPrompt(); }
}
}  // namespace tmp1x2_cli
