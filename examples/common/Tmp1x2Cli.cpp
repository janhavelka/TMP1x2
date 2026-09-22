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
  const unsigned long value = std::strtoul(text, &end, 0);
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
  return model == TMP1x2::Model::TMP102 ? "TMP102" : "TMP112";
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
uint32_t Cli::now() const { return _platform.nowMs ? _platform.nowMs(_platform.user) : 0; }
void Cli::status(TMP1x2::Status value) {
  const unsigned code = value.ok() ? 32U : value.inProgress() ? 36U : 31U;
  print("%s[%s]%s %s detail=%ld%s%s\n", color(code), value.ok() ? "I" : value.inProgress() ? "I" : "E",
        color(0), TMP1x2::errorName(value.code), static_cast<long>(value.detail),
        value.msg && *value.msg ? ": " : "", value.msg ? value.msg : "");
}
void Cli::setup(const Platform& platform, const TMP1x2::Config& config) {
  _platform = platform;
  _config = config;
  printVersion();
  print("Diagnostic CLI; application owns the bus. Type 'help' for commands.\n");
  print("Target model is selected by the user; these devices have no unique ID register.\n");
  status(_device.begin(_config));
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
  item("discover", "Probe 0x48..0x4B; ACK does not identify the chip");
  item("init / begin", "Apply and verify desired configuration");
  item("bind / unbind", "Bus-silent transport binding or full release");
  item("end", "Release driver state without changing the bus");
  item("shutdown", "Explicitly write low-power shutdown mode");
  item("addr [0x48..0x4B]", "Set target address while driver is ended");
  item("model [tmp102|tmp112]", "Set documented chip model while ended");
  item("color [0|1|off|on]", "Show or toggle ANSI colors");
  section("Data");
  item("read / temp", "Read latest register; freshness not guaranteed");
  item("start / oneshot", "Trigger single conversion in shutdown mode");
  item("poll / ready", "Check one-shot completion");
  item("tryread", "Read pending one-shot when ready");
  item("readblocking [timeout_ms]", "Bounded one-shot convenience, 1..5000 ms");
  item("sample / sampleage", "Show cached CLI sample and age without I2C");
  item("watch [N] [interval_ms]", "Finite sampling, default 20 at 1000 ms");
  item("stop", "Stop watch/stress; no I2C");
  section("Configuration");
  item("cfg / settings / snapshot", "Desired settings cached by CLI; no I2C");
  item("mode [cont|shutdown]", "Continuous conversion or low-power shutdown");
  item("rate [0.25|1|4|8]", "Conversion rate in Hz");
  item("extended [0|1|off|on]", "Select normal 12-bit or extended 13-bit format");
  item("threshold [low_C high_C]", "Read or set temperature thresholds");
  item("alert [comparator|interrupt]", "Thermostat operating mode");
  item("polarity [low|high]", "ALERT output polarity");
  item("faults [1|2|4|6]", "Consecutive faults before ALERT");
  item("verify", "Compare hardware configuration with driver cache");
  section("Registers");
  item("config / status", "Read and decode live configuration register");
  item("dump", "Read all four registers; can clear interrupt ALERT");
  item("reg / rreg <0..3>", "Read one 16-bit register (MSB first)");
  item("wreg <1..3> <0..0xFFFF>", "Raw write; configuration trust may require recover");
  section("Diagnostics");
  item("drv / health / state / online", "Cached driver health and independent bus counters");
  item("diag", "Version, desired settings and cached health");
  item("probe", "Read device configuration without driver health effects");
  item("recover", "Explicitly reapply desired configuration");
  item("stress [N]", "Finite cooperative samples, default 100; stop cancels");
  item("selfcheck / selftest", "Bounded probe/configuration/sample diagnostic");
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
  if (_platform.transferStats) {
    const auto bus = _platform.transferStats(_platform.user);
    print("Bus: attempts=%lu ok=%lu fail=%lu (includes scan/probe; independent of driver health)\n",
          static_cast<unsigned long>(bus.attempts), static_cast<unsigned long>(bus.successes),
          static_cast<unsigned long>(bus.failures));
  }
}
void Cli::stop() {
  _watch = false;
  _remaining = 0;
  print("Watch stopped: ok=%lu fail=%lu\n", static_cast<unsigned long>(_watchSuccess),
        static_cast<unsigned long>(_watchFailures));
}
void Cli::feed(char value) {
  if (value == '\r' || value == '\n') {
    if (_overflow) { print("%s[E]%s Input too long; command discarded\n", color(31), color(0)); }
    else if (_length != 0) { _line[_length] = '\0'; processCommand(_line); }
    else return;
    _length = 0;
    _overflow = false;
    printPrompt();
  } else if (value == '\b' || value == 127) {
    if (!_overflow && _length) --_length;
  } else if (static_cast<unsigned char>(value) >= 32U && !_overflow) {
    if (_length + 1 < sizeof(_line)) _line[_length++] = value;
    else _overflow = true;
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
  if ((equals(command, "scan") || equals(command, "discover")) && count == 1) {
    if (_watch) { print("Stop watch before scanning.\n"); return; }
    if (!_platform.probeAddress) { print("No scan adapter.\n"); return; }
    const uint8_t first = equals(command, "scan") ? 0x08 : 0x48;
    const uint8_t last = equals(command, "scan") ? 0x77 : 0x4B;
    unsigned found = 0;
    unsigned otherErrors = 0;
    for (uint8_t address = first; address <= last; ++address) {
      const auto st = _platform.probeAddress(address, _platform.user);
      if (st.ok()) { ++found; print("  Found 0x%02X%s\n", address, address >= 0x48 && address <= 0x4B ? " (TMP1x2-compatible address; identity unverified)" : ""); }
      else if (!st.is(TMP1x2::Err::I2C_NACK_ADDR) && !st.is(TMP1x2::Err::DEVICE_NOT_FOUND)) ++otherErrors;
    }
    print("Scan complete: %u ACK, %u other transport errors.\n", found, otherErrors); return;
  }
  if (equals(command, "stop") && count == 1) { stop(); return; }
  if ((equals(command, "drv") || equals(command, "health") || equals(command, "state") || equals(command, "online")) && count == 1) { printHealth(); return; }
  if ((equals(command, "cfg") || equals(command, "settings") || equals(command, "snapshot")) && count == 1) { printSettings(); return; }
  if (equals(command, "diag") && count == 1) { printVersion(); printSettings(); printHealth(); return; }
  if ((equals(command, "sample") || equals(command, "sampleage")) && count == 1) {
    if (!_hasSample) print("No cached CLI sample.\n");
    else { printSample(_lastSample); print("Age: %lu ms\n", static_cast<unsigned long>(now() - _lastSample.timestampMs)); }
    return;
  }
  if (_watch) { print("Watch active; use stop before other hardware commands.\n"); return; }
  if ((equals(command, "begin") || equals(command, "init")) && count == 1) { _oneShot = false; _hasSample = false; status(_device.begin(_config)); return; }
  if (equals(command, "bind") && count == 1) { _oneShot = false; _hasSample = false; status(_device.bind(_config)); return; }
  if (equals(command, "unbind") && count == 1) { _device.unbind(); _oneShot = false; _hasSample = false; print("Transport binding released without I2C.\n"); return; }
  if (equals(command, "end") && count == 1) { _device.end(); _oneShot = false; _hasSample = false; print("Driver ended; bus remains application-owned.\n"); return; }
  if (equals(command, "shutdown") && count == 1) { status(_device.shutdown()); _config = _device.getConfig(); _oneShot = false; _hasSample = false; return; }
  if (equals(command, "recover") && count == 1) { _oneShot = false; _hasSample = false; status(_device.recover()); return; }
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
    if (equals(command, "addr")) { uint32_t value = 0; if (!integer(args[1], 0x48, 0x4B, value)) { invalid(); return; } _config.i2cAddress = static_cast<uint8_t>(value); }
    else if (equals(args[1], "tmp102") || equals(args[1], "102")) _config.model = TMP1x2::Model::TMP102;
    else if (equals(args[1], "tmp112") || equals(args[1], "112")) _config.model = TMP1x2::Model::TMP112;
    else { invalid(); return; }
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
  if (equals(command, "watch") || equals(command, "stress")) {
    uint32_t amount = equals(command, "stress") ? 100 : 20;
    uint32_t interval = equals(command, "stress") ? 100 : 1000;
    if (count > 3 || (count >= 2 && !integer(args[1], 1, 100000, amount)) || (count == 3 && !integer(args[2], 1, 60000, interval))) { invalid(); return; }
    _watch = true; _remaining = amount; _intervalMs = interval; _nextMs = now(); _watchSuccess = 0; _watchFailures = 0;
    _oneShot = _device.conversionStarted(); _conversionDeadlineMs = now() + 500;
    print("Watch: %lu samples, interval=%lu ms; stop cancels. Continuous reads may repeat a sensor conversion.\n", static_cast<unsigned long>(amount), static_cast<unsigned long>(interval)); return;
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
    const auto st = _device.setThresholds(low, high); status(st);
    _config = _device.getConfig(); _hasSample = false; _oneShot = _device.conversionStarted();
    return;
  }
  if (equals(command, "mode") || equals(command, "rate") || equals(command, "extended") || equals(command, "alert") || equals(command, "polarity") || equals(command, "faults")) {
    if (count == 1) { printSettings(); return; }
    if (count != 2) { invalid(); return; }
    auto desired = _config;
    TMP1x2::Status st{};
    if (equals(command, "mode")) {
      if (equals(args[1], "cont") || equals(args[1], "continuous")) desired.mode = TMP1x2::Mode::CONTINUOUS;
      else if (equals(args[1], "shutdown")) desired.mode = TMP1x2::Mode::SHUTDOWN;
      else { invalid(); return; }
      st = _device.setMode(desired.mode);
    } else if (equals(command, "rate")) {
      if (equals(args[1], "0.25")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(0);
      else if (equals(args[1], "1")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(1);
      else if (equals(args[1], "4")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(2);
      else if (equals(args[1], "8")) desired.conversionRate = static_cast<TMP1x2::ConversionRate>(3);
      else { invalid(); return; }
      st = _device.setConversionRate(desired.conversionRate);
    } else if (equals(command, "extended")) {
      if (!boolean(args[1], desired.extendedMode)) { invalid(); return; }
      st = _device.setExtendedMode(desired.extendedMode);
    } else if (equals(command, "alert")) {
      if (equals(args[1], "comparator")) desired.alertMode = TMP1x2::AlertMode::COMPARATOR;
      else if (equals(args[1], "interrupt")) desired.alertMode = TMP1x2::AlertMode::INTERRUPT_MODE;
      else { invalid(); return; }
      st = _device.setAlertMode(desired.alertMode);
    } else if (equals(command, "polarity")) {
      if (equals(args[1], "low")) desired.alertPolarity = TMP1x2::AlertPolarity::ACTIVE_LOW;
      else if (equals(args[1], "high")) desired.alertPolarity = TMP1x2::AlertPolarity::ACTIVE_HIGH;
      else { invalid(); return; }
      st = _device.setAlertPolarity(desired.alertPolarity);
    } else {
      uint32_t faults = 0;
      if (!integer(args[1], 1, 6, faults) || faults == 3 || faults == 5) { invalid(); return; }
      desired.faultQueue = static_cast<TMP1x2::FaultQueue>(faults == 1 ? 0 : faults == 2 ? 1 : faults == 4 ? 2 : 3);
      st = _device.setFaultQueue(desired.faultQueue);
    }
    status(st);
    // Core retains validated desired settings on ambiguous I2C failure so an
    // explicit recover can reapply them. Keep the CLI view consistent.
    _config = _device.getConfig(); _hasSample = false; _oneShot = _device.conversionStarted();
    return;
  }
  invalid();
}
void Cli::tick() {
  const uint32_t nowMs = now();
  if (!_watch) _device.tick(nowMs);
  if (!_watch || static_cast<int32_t>(nowMs - _nextMs) < 0) return;
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
        status(TMP1x2::Status::Error(TMP1x2::Err::TIMEOUT, "watch conversion deadline; recover before restarting"));
        ++_watchFailures; stop(); printPrompt(); return;
      }
      _oneShot = false;
    }
  } else st = _device.readSample(sample);
  if (st.ok()) { ++_watchSuccess; printSample(sample); }
  else { ++_watchFailures; status(st); }
  _nextMs = nowMs + _intervalMs;
  if (--_remaining == 0) { stop(); printPrompt(); }
}
}  // namespace tmp1x2_cli
