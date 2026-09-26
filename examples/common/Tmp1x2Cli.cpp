#include "Tmp1x2Cli.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tmp1x2_cli {
namespace {
bool equals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
bool integer(const char* text, uint32_t low, uint32_t high, uint32_t& out) {
  if (text == nullptr || *text == '\0' || *text == '-' || *text == '+') return false;
  errno = 0;
  char* end = nullptr;
  const int base = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
  const unsigned long value = std::strtoul(text, &end, base);
  if (errno != 0 || *end != '\0' || value < low || value > high) return false;
  out = static_cast<uint32_t>(value);
  return true;
}
bool real(const char* text, float& out) {
  if (text == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  out = std::strtof(text, &end);
  return errno == 0 && *end == '\0' && std::isfinite(out);
}
bool boolean(const char* text, bool& out) {
  if (equals(text, "1") || equals(text, "on")) { out = true; return true; }
  if (equals(text, "0") || equals(text, "off")) { out = false; return true; }
  return false;
}
const char* modelName(TMP1x2::Model model) {
  return TMP1x2::toString(model);
}
}  // namespace

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
void Cli::print(const char* format, ...) {
  if (_platform.vprintf == nullptr) return;
  va_list args;
  va_start(args, format);
  _platform.vprintf(_platform.user, format, args);
  va_end(args);
}
uint32_t Cli::now() const {
  return _platform.nowMs ? _platform.nowMs(_platform.user) : _config.nowMs ? _config.nowMs(_config.timeUser) : 0;
}
void Cli::status(TMP1x2::Status value) {
  const unsigned code = value.ok() ? 32U : value.inProgress() ? 36U : 31U;
  print("%s[%s]%s %s detail=%ld%s%s\n", color(code), value.ok() ? "I" : value.inProgress() ? "I" : "E",
        color(0), TMP1x2::errorName(value.code), static_cast<long>(value.detail),
        value.msg && *value.msg ? ": " : "", value.msg ? value.msg : "");
}
void Cli::setup(const Platform& platform, const TMP1x2::Config& config) {
  _platform = platform;
  _config = config;
  _configuredAlertPin = config.alertPin;
  _configuredGpioRead = config.gpioRead;
  _configuredGpioUser = config.gpioUser;
  printVersion();
  print("Diagnostic CLI; application owns the bus. Type 'help' for commands.\n");
  print("Target model is selected by the user; these devices have no unique ID register.\n");
  startOperation(TMP1x2::OperationKind::INITIALIZE);
  printPrompt();
}
void Cli::printPrompt() { print("> "); }
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
  item("measure / request", "Start cooperative sample operation");
  item("start / oneshot", "Trigger single conversion in shutdown mode");
  item("poll / ready", "Check one-shot completion");
  item("tryread", "Read pending one-shot when ready");
  item("readblocking [timeout_ms]", "Bounded one-shot convenience, 1..5000 ms");
  item("sample / sampleage", "Show cached CLI sample and age without I2C");
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
  item("stress [N]", "Finite cooperative samples, default 100; stop cancels");
  item("stress_mix [N]", "Finite probe/config/threshold/sample cycles; default 100");
  item("selfcheck / selftest", "Bounded probe/configuration/sample diagnostic");
  item("xfer_stats / counters", "Independent cached adapter transfer counters");
  item("xfer_reset", "Reset adapter counters while idle; preserves health");
  print("\nRegister reads may acknowledge interrupt-mode ALERT. No automatic bus reset.\n");
}
void Cli::printSample(const TMP1x2::Sample& sample) {
  _lastSample = sample;
  _hasSample = true;
  print("Temperature: %s%.4f C%s raw=0x%04X counts=%d extended=%s timestamp=%lu ms\n",
        color(32), static_cast<double>(sample.celsius), color(0), static_cast<unsigned>(sample.raw),
        static_cast<int>(sample.counts), sample.extendedMode ? "yes" : "no",
        static_cast<unsigned long>(sample.timestampMs));
}
void Cli::printSettings() {
  print("Desired: model=%s address=0x%02X timeout=%lu ms mode=%s rate=%s extended=%s\n",
        modelName(_config.model), _config.i2cAddress, static_cast<unsigned long>(_config.i2cTimeoutMs),
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
void Cli::startOperation(TMP1x2::OperationKind kind, const TMP1x2::Config* desired) {
  if (!_platform.nowMs && !_config.nowMs) {
    status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "cooperative operations require a monotonic clock")); return;
  }
  TMP1x2::Status st{};
  if (kind == TMP1x2::OperationKind::INITIALIZE) {
    st = _device.bind(_config);
    if (!st.ok()) { status(st); return; }
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
  printOperationResult();
  printPrompt();
}
void Cli::printOperation() {
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
void Cli::feed(char value) {
  if (value == '\r' || value == '\n') {
    if (_invalidInput) { print("%s[E]%s Invalid control character; command discarded\n", color(31), color(0)); }
    else if (_overflow) { print("%s[E]%s Input too long; command discarded\n", color(31), color(0)); }
    else if (_length != 0) { _line[_length] = '\0'; processCommand(_line); }
    else return;
    _length = 0;
    _overflow = false;
    _invalidInput = false;
    printPrompt();
  } else if (value == '\b' || value == 127) {
    if (!_overflow && !_invalidInput && _length) --_length;
  } else if (value == '\t' || static_cast<unsigned char>(value) >= 32U) {
    if (!_overflow && !_invalidInput) {
      if (_length + 1 < sizeof(_line)) _line[_length++] = value;
      else _overflow = true;
    }
  } else {
    // Do not turn a malformed token into a valid mutation by deleting bytes.
    _invalidInput = true;
  }
}
void Cli::processCommand(const char* text) {
  char buffer[160];
  if (std::strlen(text) >= sizeof(buffer)) { status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "line too long")); return; }
  std::strcpy(buffer, text);
  char* args[5]{};
  size_t count = 0;
  char* cursor = buffer;
  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor == '\0') break;
    if (count == 5) { status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "too many arguments")); return; }
    args[count++] = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
    if (*cursor) *cursor++ = '\0';
  }
  if (!count) return;
  const char* command = args[0];
  const auto invalid = [this]() { status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "invalid command/arguments; see help")); };
  if ((equals(command, "help") || equals(command, "?")) && count == 1) { printHelp(); return; }
  if ((equals(command, "version") || equals(command, "ver")) && count == 1) { printVersion(); return; }
  if (equals(command, "color")) {
    bool value = _color;
    if (count > 2 || (count == 2 && !boolean(args[1], value))) { invalid(); return; }
    _color = value; print("Color: %s\n", _color ? "on" : "off"); return;
  }
  if (equals(command, "verbose") || equals(command, "quiet")) {
    const bool quiet = equals(command, "quiet");
    bool value = quiet ? !_verbose : _verbose;
    if (count > 2 || (count == 2 && !boolean(args[1], value))) { invalid(); return; }
    _verbose = quiet ? !value : value;
    print("Verbose: %s (watch/stress samples and immediate errors)\n", _verbose ? "on" : "off"); return;
  }
  if (equals(command, "stats")) {
    if (count == 1) { printRunStats(); return; }
    if (count != 2 || !equals(args[1], "reset")) { invalid(); return; }
    if (_watch) { print("Stop sampling before resetting run statistics.\n"); return; }
    _run = RunStats{};
    print("Run statistics reset; sample, adapter counters, and driver health retained.\n"); return;
  }
  if ((equals(command, "xfer_stats") || equals(command, "counters")) && count == 1) { printTransferStats(); return; }
  if (equals(command, "xfer_reset") && count == 1) {
    if (_watch || _device.operationActive()) { print("Stop active work before resetting adapter counters.\n"); return; }
    if (!_platform.resetTransferStats) { print("No adapter counter-reset callback.\n"); return; }
    _platform.resetTransferStats(_platform.user);
    print("Adapter counters reset; driver health retained.\n"); return;
  }
  if (equals(command, "job")) {
    if (count == 1 || (count == 2 && equals(args[1], "status"))) { printOperation(); return; }
    if (count == 2 && equals(args[1], "result")) { printOperationResult(); return; }
    if (count == 2 && equals(args[1], "cancel")) { status(_device.cancel()); finishOperation(); return; }
    invalid(); return;
  }
  if (equals(command, "result") && count == 1) { printOperationResult(); return; }
  if (equals(command, "cancel") && count == 1) { status(_device.cancel()); finishOperation(); return; }
  if (equals(command, "timing") && count == 1) { printTiming(); return; }
  if (equals(command, "convert")) {
    uint32_t raw = 0;
    if (count != 2 || !integer(args[1], 0, 65535, raw)) { invalid(); return; }
    TMP1x2::Sample sample{}; const auto st = TMP1x2::TMP1x2::decodeTemperature(static_cast<uint16_t>(raw), sample);
    if (!st.ok()) status(st);
    else print("Decoded: %.4f C counts=%d format=%s (pure conversion; no acquired sample)\n",
               static_cast<double>(sample.celsius), static_cast<int>(sample.counts), sample.extendedMode ? "extended" : "normal");
    return;
  }
  if (equals(command, "thcalc") || equals(command, "thdecode")) {
    float value = 0; uint32_t word = 0; bool extended = _config.extendedMode;
    const bool decode = equals(command, "thdecode");
    if ((count != 2 && count != 3) || (decode ? !integer(args[1], 0, 65535, word) : !real(args[1], value))) { invalid(); return; }
    if (count == 3) {
      if (equals(args[2], "normal")) extended = false;
      else if (equals(args[2], "extended")) extended = true;
      else { invalid(); return; }
    }
    uint16_t raw = static_cast<uint16_t>(word);
    const auto st = decode ? TMP1x2::Status::Ok() : TMP1x2::TMP1x2::encodeThreshold(value, extended, raw);
    if (!st.ok()) status(st);
    else print("Threshold: 0x%04X = %.4f C format=%s\n", static_cast<unsigned>(raw),
               static_cast<double>(TMP1x2::TMP1x2::decodeThreshold(raw, extended)), extended ? "extended" : "normal");
    return;
  }
  if ((equals(command, "alertpin") || equals(command, "intpin")) && count == 1) {
    bool active = false; const auto st = _device.readAlertPin(active);
    if (st.ok()) print("Physical ALERT: %s%s%s pin=%d (GPIO only; no I2C)\n", color(active ? 33U : 32U),
                       active ? "active" : "inactive", color(0), _config.alertPin);
    else status(st);
    return;
  }
  if ((equals(command, "scan") || equals(command, "discover")) && count == 1) {
    if (_watch || _device.operationActive()) { print("Stop active work before scanning.\n"); return; }
    if (!_platform.probeAddress) { print("No scan adapter.\n"); return; }
    const uint8_t first = equals(command, "scan") ? 0x08 : 0x40;
    const uint8_t last = equals(command, "scan") ? 0x77 : 0x4B;
    unsigned found = 0;
    unsigned otherErrors = 0;
    for (uint8_t address = first; address <= last; ++address) {
      if (equals(command, "discover") && address >= 0x44 && address <= 0x47) continue;
      const auto st = _platform.probeAddress(address, _platform.user);
      if (st.ok()) {
        ++found;
        print("  Found 0x%02X%s\n", address,
              address >= 0x40 && address <= 0x43 ? " (TMP112D ADD0-compatible; identity unverified)" :
              address >= 0x48 && address <= 0x4B ? " (TMP102/TMP112-compatible; identity unverified)" : "");
      }
      else if (!st.is(TMP1x2::Err::I2C_NACK_ADDR) && !st.is(TMP1x2::Err::DEVICE_NOT_FOUND)) ++otherErrors;
    }
    print("Scan complete: %u ACK, %u other transport errors.\n", found, otherErrors); return;
  }
  if (equals(command, "stop") && count == 1) {
    if (_device.operationActive()) { status(_device.cancel()); finishOperation(); }
    else stop();
    return;
  }
  if ((equals(command, "drv") || equals(command, "health") || equals(command, "state") || equals(command, "online")) && count == 1) { printHealth(); return; }
  if ((equals(command, "cfg") || equals(command, "settings") || equals(command, "snapshot")) && count == 1) { printSettings(); return; }
  if (equals(command, "diag") && count == 1) { printVersion(); printSettings(); printHealth(); printOperation(); printRunStats(); return; }
  if ((equals(command, "sample") || equals(command, "sampleage")) && count == 1) {
    if (!_hasSample) print("No cached CLI sample.\n");
    else { printSample(_lastSample); print("Age: %lu ms\n", static_cast<unsigned long>(now() - _lastSample.timestampMs)); }
    return;
  }
  if (_watch) { print("Watch active; use stop before other hardware commands.\n"); return; }
  if (_device.operationActive()) { print("Operation active; use job, result, or cancel before other hardware commands.\n"); return; }
  if ((equals(command, "begin") || equals(command, "init")) && count == 1) {
    startOperation(TMP1x2::OperationKind::INITIALIZE);
    return;
  }
  if (equals(command, "bind") && count == 1) {
    _oneShot = false; _hasSample = false; status(_device.bind(_config));
    if (_device.isBound()) _config = _device.getConfig();
    return;
  }
  if (equals(command, "unbind") && count == 1) { _device.unbind(); _oneShot = false; _hasSample = false; print("Transport binding released without I2C.\n"); return; }
  if (equals(command, "end") && count == 1) { _device.end(); _oneShot = false; _hasSample = false; print("Driver ended; bus remains application-owned.\n"); return; }
  if (equals(command, "shutdown") && count == 1) {
    startOperation(TMP1x2::OperationKind::SHUTDOWN); return;
  }
  if (equals(command, "recover") && count == 1) { startOperation(TMP1x2::OperationKind::RECOVER); return; }
  if ((equals(command, "measure") || equals(command, "request")) && count == 1) { startOperation(TMP1x2::OperationKind::READ); return; }
  if (equals(command, "probe") && count == 1) { status(_device.probe()); return; }
  if (equals(command, "verify") && count == 1) { status(_device.verifyConfiguration()); return; }
  if ((equals(command, "selfcheck") || equals(command, "selftest")) && count == 1) {
    auto st = _device.probe(); status(st);
    if (st.ok()) { st = _device.verifyConfiguration(); status(st); }
    if (st.ok()) { TMP1x2::Sample sample{}; st = _device.readSample(sample); status(st); if (st.ok()) printSample(sample); }
    printHealth(); return;
  }
  if (equals(command, "addr") || equals(command, "model")) {
    if (count == 1) { printSettings(); return; }
    if (count != 2 || _device.state() != TMP1x2::DriverState::UNINIT) { print("Use end before selecting address/model.\n"); return; }
    auto selected = _config;
    if (equals(command, "addr")) {
      uint32_t value = 0;
      if (!integer(args[1], TMP1x2::modelAddressMin(selected.model), TMP1x2::modelAddressMax(selected.model), value)) { invalid(); return; }
      selected.i2cAddress = static_cast<uint8_t>(value);
    }
    else if (equals(args[1], "tmp102") || equals(args[1], "102")) selected.model = TMP1x2::Model::TMP102;
    else if (equals(args[1], "tmp112") || equals(args[1], "112")) selected.model = TMP1x2::Model::TMP112;
    else if (equals(args[1], "tmp112d") || equals(args[1], "112d")) selected.model = TMP1x2::Model::TMP112D_ADDRESS_SELECT;
    else { invalid(); return; }
    if (!TMP1x2::isValidAddress(selected.model, selected.i2cAddress))
      selected.i2cAddress = TMP1x2::modelAddressMin(selected.model);
    selected.alertPin = TMP1x2::hasAlertOutput(selected.model) ? _configuredAlertPin : -1;
    selected.gpioRead = TMP1x2::hasAlertOutput(selected.model) ? _configuredGpioRead : nullptr;
    selected.gpioUser = TMP1x2::hasAlertOutput(selected.model) ? _configuredGpioUser : nullptr;
    // Address/model selection also updates the bus-silent binding so probe or
    // recover cannot silently access the previously selected device.
    const auto st = _device.bind(selected);
    if (!st.ok()) { status(st); return; }
    _config = _device.getConfig();
    _oneShot = false; _hasSample = false;
    printSettings(); print("Run begin to apply.\n"); return;
  }
  if ((equals(command, "read") || equals(command, "temp") || equals(command, "tryread") || equals(command, "readblocking")) && count <= 2) {
    uint32_t timeout = 500;
    if ((count == 2 && !equals(command, "readblocking")) || (count == 2 && !integer(args[1], 1, 5000, timeout))) { invalid(); return; }
    TMP1x2::Sample sample{};
    auto st = equals(command, "tryread") ? _device.tryRead(sample) : equals(command, "readblocking") ? _device.readBlocking(sample, timeout) : _device.readSample(sample);
    _oneShot = _device.conversionStarted();
    if (st.ok()) printSample(sample); else status(st);
    return;
  }
  if ((equals(command, "start") || equals(command, "oneshot")) && count == 1) { const auto st = _device.startOneShot(); _oneShot = _device.conversionStarted(); _conversionDeadlineMs = now() + 500; status(st); return; }
  if ((equals(command, "poll") || equals(command, "ready")) && count == 1) { bool ready = false; const auto st = _device.isConversionReady(ready); if (st.ok()) print("Ready: %s\n", ready ? "yes" : "no"); else status(st); return; }
  if (equals(command, "watch") || equals(command, "stress") || equals(command, "stress_mix")) {
    const bool mixed = equals(command, "stress_mix");
    const bool stress = mixed || equals(command, "stress");
    uint32_t amount = stress ? 100 : 20;
    uint32_t interval = stress ? 100 : 1000;
    if (count > (mixed ? 2U : 3U) || (count >= 2 && !integer(args[1], 1, 100000, amount)) || (count == 3 && !integer(args[2], 1, 60000, interval))) { invalid(); return; }
    if (!_platform.nowMs || !_config.nowMs) {
      status(TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "watch requires platform and driver nowMs callbacks")); return;
    }
    _run = RunStats{}; _run.available = true; _run.mixed = mixed; _run.target = amount; _run.startedMs = now();
    _mixedPhase = 0;
    _run.healthSuccessBefore = _device.totalSuccess(); _run.healthFailureBefore = _device.totalFailures();
    _run.busAvailable = _platform.transferStats != nullptr;
    if (_run.busAvailable) _run.busBefore = _platform.transferStats(_platform.user);
    _watch = true; _remaining = amount; _intervalMs = interval; _nextMs = now();
    _oneShot = _device.conversionStarted(); _conversionDeadlineMs = now() + 500;
    if (mixed)
      print("Mixed stress: %lu cycles, interval=%lu ms; probe/config/threshold/sample; shutdown samples start or join one-shots. Stop cancels.\n",
            static_cast<unsigned long>(amount), static_cast<unsigned long>(interval));
    else
      print("Watch: %lu samples, interval=%lu ms; stop cancels. Continuous reads may repeat a sensor conversion.\n", static_cast<unsigned long>(amount), static_cast<unsigned long>(interval));
    return;
  }
  if (equals(command, "rawread") || equals(command, "rawwrite") || equals(command, "rawdump")) {
    uint32_t reg = 0, value = 0;
    if (equals(command, "rawdump")) {
      if (count != 1) { invalid(); return; }
      print("Raw register dump (bypasses driver health; reads may acknowledge ALERT):\n");
      for (uint8_t index = 0; index < 4; ++index) {
        uint16_t raw = 0; const auto st = _device.readRegisterRaw(index, raw);
        if (!st.ok()) { status(st); break; }
        print("0x%02X = 0x%04X\n", static_cast<unsigned>(index), static_cast<unsigned>(raw));
      }
    } else if (equals(command, "rawwrite")) {
      if (count != 3 || !integer(args[1], 1, 3, reg) || !integer(args[2], 0, 65535, value)) { invalid(); return; }
      status(_device.writeRegisterRaw(static_cast<uint8_t>(reg), static_cast<uint16_t>(value)));
      _hasSample = false; _oneShot = _device.conversionStarted();
      print("Raw write bypasses driver health; attempted writes mark configuration dirty.\n");
    } else {
      if (count != 2 || !integer(args[1], 0, 3, reg)) { invalid(); return; }
      uint16_t raw = 0; const auto st = _device.readRegisterRaw(static_cast<uint8_t>(reg), raw);
      if (st.ok()) print("Raw 0x%02X = 0x%04X (bypasses driver health)\n", static_cast<unsigned>(reg), static_cast<unsigned>(raw));
      else status(st);
    }
    return;
  }
  if (equals(command, "config") || equals(command, "status") || equals(command, "dump") || equals(command, "reg") || equals(command, "rreg") || equals(command, "wreg")) {
    if ((equals(command, "config") || equals(command, "status")) && count == 1) {
      TMP1x2::ConfigurationInfo info{};
      const auto st = _device.readConfiguration(info);
      if (!st.ok()) { status(st); return; }
      print("CONFIG=0x%04X valid=%s OS=%u EM=%u AL=%u ALERT-active=%s mode=%s rate=%s\n",
            info.raw, info.valid ? "yes" : "no", info.oneShotReady ? 1U : 0U,
            info.extendedMode ? 1U : 0U, info.alert ? 1U : 0U, info.alertActive ? "yes" : "no",
            TMP1x2::toString(info.mode), TMP1x2::toString(info.conversionRate));
      print("Thermostat=%s polarity=%s faults=%s (this read may acknowledge interrupt-mode ALERT)\n",
            TMP1x2::toString(info.alertMode), TMP1x2::toString(info.alertPolarity), TMP1x2::toString(info.faultQueue));
      return;
    }
    uint32_t reg = 1;
    uint32_t value = 0;
    if (equals(command, "dump") && count == 1) {
      for (uint8_t address = 0; address < 4; ++address) { uint16_t raw = 0; const auto st = _device.readRegister(address, raw); if (!st.ok()) { status(st); break; } print("0x%02X = 0x%04X\n", address, raw); }
      return;
    }
    if (equals(command, "wreg")) {
      if (count != 3 || !integer(args[1], 1, 3, reg) || !integer(args[2], 0, 65535, value)) { invalid(); return; }
      status(_device.writeRegister(static_cast<uint8_t>(reg), static_cast<uint16_t>(value))); _hasSample = false; return;
    }
    if (equals(command, "reg") || equals(command, "rreg")) { if (count != 2 || !integer(args[1], 0, 3, reg)) { invalid(); return; } }
    else if (count != 1) { invalid(); return; }
    uint16_t raw = 0; const auto st = _device.readRegister(static_cast<uint8_t>(reg), raw);
    if (st.ok()) print("0x%02X = 0x%04X\n", static_cast<unsigned>(reg), raw); else status(st);
    return;
  }
  if (equals(command, "threshold")) {
    if (count == 1) {
      float low = 0; float high = 0;
      const auto st = _device.readThresholds(low, high);
      if (st.ok()) print("Hardware thresholds: low=%.4f C high=%.4f C\n", static_cast<double>(low), static_cast<double>(high));
      else status(st);
      return;
    }
    float low = 0; float high = 0;
    if (count != 3 || !real(args[1], low) || !real(args[2], high)) { invalid(); return; }
    auto desired = _config; desired.lowThresholdC = low; desired.highThresholdC = high;
    startOperation(TMP1x2::OperationKind::CONFIGURE, &desired);
    return;
  }
  if (equals(command, "mode") || equals(command, "rate") || equals(command, "extended") || equals(command, "alert") || equals(command, "polarity") || equals(command, "faults")) {
    if (count == 1) { printSettings(); return; }
    if (count != 2) { invalid(); return; }
    auto desired = _config;
    if (equals(command, "mode")) {
      if (equals(args[1], "cont") || equals(args[1], "continuous")) desired.mode = TMP1x2::Mode::CONTINUOUS;
      else if (equals(args[1], "shutdown")) desired.mode = TMP1x2::Mode::SHUTDOWN;
      else { invalid(); return; }
    } else if (equals(command, "rate")) {
      if (equals(args[1], "0.25")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(0);
      else if (equals(args[1], "1")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(1);
      else if (equals(args[1], "4")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(2);
      else if (equals(args[1], "8")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(3);
      else { invalid(); return; }
    } else if (equals(command, "extended")) {
      if (!boolean(args[1], desired.extendedMode)) { invalid(); return; }
    } else if (equals(command, "alert")) {
      if (equals(args[1], "comparator")) desired.alertMode = TMP1x2::AlertMode::COMPARATOR;
      else if (equals(args[1], "interrupt")) desired.alertMode = TMP1x2::AlertMode::INTERRUPT_MODE;
      else { invalid(); return; }
    } else if (equals(command, "polarity")) {
      if (equals(args[1], "low")) desired.alertPolarity = TMP1x2::AlertPolarity::ACTIVE_LOW;
      else if (equals(args[1], "high")) desired.alertPolarity = TMP1x2::AlertPolarity::ACTIVE_HIGH;
      else { invalid(); return; }
    } else {
      uint32_t faults = 0;
      if (!integer(args[1], 1, 6, faults) || faults == 3 || faults == 5) { invalid(); return; }
      desired.faultQueue = static_cast<TMP1x2::FaultQueue>(faults == 1 ? 0 : faults == 2 ? 1 : faults == 4 ? 2 : 3);
    }
    startOperation(TMP1x2::OperationKind::CONFIGURE, &desired);
    return;
  }
  invalid();
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
