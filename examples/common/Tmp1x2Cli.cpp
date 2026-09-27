#include "Tmp1x2Cli.h"

// Shared input parsing, command dispatch and application-session setup.

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
}  // namespace

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
  if (activeWork() || _device.conversionStarted() || _device.resultPending()) {
    status(TMP1x2::Status::Error(TMP1x2::Err::BUSY, "CLI setup cannot replace active work or a pending conversion")); return;
  }
  auto valid = TMP1x2::TMP1x2::validateConfig(config);
  if (valid.ok() && !platform.nowMs && !config.nowMs)
    valid = TMP1x2::Status::Error(TMP1x2::Err::INVALID_CONFIG, "cooperative operations require a monotonic clock");
  if (!valid.ok()) {
    if (!_configured) _platform = platform;
    status(valid); return;
  }
  _platform = platform;
  _config = config;
  _configured = true;
  _length = 0; _overflow = false; _invalidInput = false;
  _diagnostic = Diagnostic{}; _lastDiagnostic = Diagnostic{}; _run = RunStats{};
  _lastOperation = TMP1x2::OperationResult{}; _hasOperationResult = false; _lastResultDiagnostic = false;
  _operationToken = 0; _hasSample = false; _oneShot = false;
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
  // Refresh caller-owned time without polling a retained manual conversion.
  if (_platform.nowMs && !_device.operationActive()) (void)_device.poll(now(), 0);
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
    if (activeWork()) { print("Stop active work before resetting adapter counters.\n"); return; }
    if (!_platform.resetTransferStats) { print("No adapter counter-reset callback.\n"); return; }
    _platform.resetTransferStats(_platform.user);
    print("Adapter counters reset; driver health retained.\n"); return;
  }
  if (equals(command, "healthreset") && count == 1) {
    if (activeWork()) { print("Stop active work before resetting health totals.\n"); return; }
    _device.resetStatistics();
    print("Cumulative driver totals reset; online state, consecutive failures, error and timestamps retained.\n"); return;
  }
  if (equals(command, "job")) {
    if (count == 1 || (count == 2 && equals(args[1], "status"))) { printOperation(); return; }
    if (count == 2 && equals(args[1], "result")) { printOperationResult(); return; }
    if (count == 2 && equals(args[1], "cancel")) { cancelWork(); return; }
    invalid(); return;
  }
  if (equals(command, "result") && count == 1) { printOperationResult(); return; }
  if (equals(command, "cancel") && count == 1) { cancelWork(); return; }
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
    startDiagnostic(equals(command, "scan") ? DiagnosticKind::SCAN : DiagnosticKind::DISCOVER); return;
  }
  if (equals(command, "stop") && count == 1) { cancelWork(); return; }
  if ((equals(command, "drv") || equals(command, "health") || equals(command, "state") || equals(command, "online")) && count == 1) { printHealth(); return; }
  if ((equals(command, "cfg") || equals(command, "settings") || equals(command, "snapshot")) && count == 1) { printSettings(); return; }
  if (equals(command, "settings") && count == 2 && equals(args[1], "values")) {
    print("mode: cont|shutdown; rate: 0.25|1|4|8; extended: 0|1|off|on\n");
    print("alert: comparator|interrupt; polarity: low|high; faults: 1|2|4|6\n");
    print("threshold: low_C high_C, low <= high, rounded to 0.0625 C; normal [-128,127.9375], extended [-256,255.9375].\n"); return;
  }
  if (equals(command, "settings") && count == 2 && (equals(args[1], "read") || equals(args[1], "live"))) {
    startDiagnostic(DiagnosticKind::SETTINGS); return;
  }
  if (equals(command, "snapshot") && count == 2 && equals(args[1], "read")) { startDiagnostic(DiagnosticKind::SNAPSHOT); return; }
  if (equals(command, "diag") && count == 1) { printVersion(); printSettings(); printHealth(); printOperation(); printRunStats(); return; }
  if ((equals(command, "sample") || equals(command, "sampleage")) && count == 1) {
    if (!_hasSample) print("No cached CLI sample.\n");
    else {
      printSample(_lastSample);
      if (_lastSample.timestampValid && (_platform.nowMs || _config.nowMs))
        print("Age: %lu ms\n", static_cast<unsigned long>(now() - _lastSample.timestampMs));
      else print("Age: unknown (no acquisition clock).\n");
    }
    return;
  }
  if (equals(command, "freshness")) {
    uint32_t limit = 1000;
    if (count > 2 || (count == 2 && !integer(args[1], 0, INT32_MAX, limit))) { invalid(); return; }
    if (!_hasSample) { print("No cached CLI sample.\n"); return; }
    if (!_lastSample.timestampValid || (!_platform.nowMs && !_config.nowMs)) {
      print("Acquisition: timestamp=unknown age=unknown max=%lu ms usable=no; fresh-conversion=%s\n",
            static_cast<unsigned long>(limit), _lastSample.freshConversion ? "confirmed one-shot" : "unproven (latest register)"); return;
    }
    print("Acquisition: timestamp=%s age=%lu ms max=%lu ms usable=%s; fresh-conversion=%s\n",
          _lastSample.timestampValid ? "known" : "unknown", static_cast<unsigned long>(now() - _lastSample.timestampMs),
          static_cast<unsigned long>(limit), _device.sampleFresh(now(), limit) ? "yes" : "no",
          _lastSample.freshConversion ? "confirmed one-shot" : "unproven (latest register)"); return;
  }
  if (count == 1 && (equals(command, "mode") || equals(command, "rate") || equals(command, "extended") ||
      equals(command, "alert") || equals(command, "polarity") || equals(command, "faults") || equals(command, "addr") || equals(command, "model"))) {
    printSettings(); return;
  }
  if (_diagnostic.active) { print("Diagnostic active; use job, result, or cancel before other hardware commands.\n"); return; }
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
  if (equals(command, "selfcheck") || equals(command, "selftest")) {
    if (count == 1) startDiagnostic(DiagnosticKind::SELFCHECK);
    else if (equals(command, "selftest") && count == 2 && equals(args[1], "full")) startDiagnostic(DiagnosticKind::CONFIGTEST);
    else invalid();
    return;
  }
  if (equals(command, "reset") && count == 1) {
    print("No per-device reset exists. Use busreset or reset all to reset ALL compatible devices on the bus.\n"); return;
  }
  if ((equals(command, "busreset") && count == 1) ||
      (equals(command, "reset") && count == 2 && equals(args[1], "all"))) {
    if (!_platform.busWrite) { print("No general-call write adapter.\n"); return; }
    if (_device.isBound()) {
      const auto st = _device.invalidateDeviceState();
      if (!st.ok()) { status(st); return; }
    }
    _hasSample = false; _oneShot = false; _hasOperationResult = false;
    print("General-call RESET: ALL compatible devices on this bus may reset. Reinitialize every affected driver, including after an error.\n");
    status(TMP1x2::BusOperations::generalCallReset(_platform.busWrite, _platform.user, _config.i2cTimeoutMs));
    print("Local configuration remains invalid until explicit begin/recover completes.\n"); return;
  }
  if ((equals(command, "ara") || equals(command, "alertresponse")) && count == 1) {
    TMP1x2::BusOperations::AlertResponse response{};
    print("ARA acknowledges one alerting device; even a failed response can clear its hardware alert.\n");
    const auto st = TMP1x2::BusOperations::readAlertResponse(_platform.busReceive, _platform.user, _config.i2cTimeoutMs, response);
    if (!st.ok()) status(st);
    else {
      print("Alert response: raw=0x%02X address=0x%02X status-bit=%u (identity unverified; model-specific meaning)\n",
            response.raw, response.address, response.alertStatusBit ? 1U : 0U);
      if (response.address == _config.i2cAddress && _device.isInitialized() && !_device.hardwareConfigDirty()) {
        TMP1x2::BusOperations::AlertCause cause{};
        if (TMP1x2::BusOperations::decodeAlertCause(response, _config.model, _config.alertPolarity, cause).ok())
          print("  Cause for selected BOM model %s and cached polarity: %s\n", TMP1x2::toString(_config.model),
                TMP1x2::BusOperations::toString(cause));
      }
    }
    return;
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
  if ((equals(command, "read") || equals(command, "temp") || equals(command, "tempf") || equals(command, "tryread") || equals(command, "readblocking")) && count <= 2) {
    uint32_t timeout = 500;
    if ((count == 2 && !equals(command, "readblocking")) || (count == 2 && !integer(args[1], 1, 5000, timeout))) { invalid(); return; }
    TMP1x2::Sample sample{};
    auto st = equals(command, "tryread") ? _device.tryRead(sample) : equals(command, "readblocking") ? _device.readBlocking(sample, timeout) : _device.readSample(sample);
    _oneShot = _device.conversionStarted();
    if (st.ok()) {
      if (equals(command, "tempf")) {
        _lastSample = sample; _hasSample = true;
        print("Temperature: %.4f F raw=0x%04X (latest register)\n",
              static_cast<double>(TMP1x2::TMP1x2::celsiusToFahrenheit(sample.celsius)), static_cast<unsigned>(sample.raw));
      } else printSample(sample);
    } else status(st);
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
  if (equals(command, "config") || equals(command, "status") || equals(command, "status_raw") || equals(command, "raw") || equals(command, "dump") || equals(command, "reg") || equals(command, "rreg") || equals(command, "wreg")) {
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
    uint32_t reg = equals(command, "raw") ? 0U : 1U;
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
}  // namespace tmp1x2_cli
