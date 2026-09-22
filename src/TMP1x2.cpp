/// @file TMP1x2.cpp
#include "TMP1x2/TMP1x2.h"
#include <cmath>
#include <limits>

namespace TMP1x2 {
namespace {
Status notReady() {
  return Status::Error(Err::MEASUREMENT_NOT_READY, "No completed measurement available");
}
bool healthFailure(Err code) {
  return code == Err::I2C_ERROR || code == Err::I2C_NACK_ADDR ||
         code == Err::I2C_NACK_DATA || code == Err::I2C_TIMEOUT ||
         code == Err::I2C_BUS || code == Err::DEVICE_NOT_FOUND || code == Err::TIMEOUT;
}
Status terminalTransportStatus(Status status) {
  if (status.inProgress())
    return Status::Error(Err::I2C_ERROR, "Blocking transport returned IN_PROGRESS", status.detail);
  return status;
}
int16_t signedCounts(uint16_t raw, bool extended) {
  const int32_t code = static_cast<int32_t>(raw >> (extended ? 3 : 4));
  const int32_t sign = extended ? 4096 : 2048;
  return static_cast<int16_t>((code & sign) ? code - 2 * sign : code);
}
} // namespace

Status TMP1x2::validateConfig(const Config& c) {
  if (!c.i2cWrite || !c.i2cWriteRead)
    return Status::Error(Err::INVALID_CONFIG, "Both I2C callbacks are required");
  if (c.i2cAddress < cmd::I2C_ADDR_MIN || c.i2cAddress > cmd::I2C_ADDR_MAX)
    return Status::Error(Err::INVALID_CONFIG, "Address must be 0x48..0x4B");
  if (c.i2cTimeoutMs == 0 || c.i2cTimeoutMs > 0x7FFFFFFFu)
    return Status::Error(Err::INVALID_CONFIG, "I2C timeout must be 1..INT32_MAX ms");
  if (static_cast<uint8_t>(c.model) > 1 || static_cast<uint8_t>(c.mode) > 1 ||
      static_cast<uint8_t>(c.conversionRate) > 3 || static_cast<uint8_t>(c.alertMode) > 1 ||
      static_cast<uint8_t>(c.alertPolarity) > 1 || static_cast<uint8_t>(c.faultQueue) > 3)
    return Status::Error(Err::INVALID_CONFIG, "Invalid configuration enum");
  if (c.alertPin < -1 || (c.alertPin >= 0 && !c.gpioRead))
    return Status::Error(Err::INVALID_CONFIG, "Configured ALERT pin requires GPIO callback");
  uint16_t low = 0, high = 0;
  if (!encodeThreshold(c.lowThresholdC, c.extendedMode, low).ok() ||
      !encodeThreshold(c.highThresholdC, c.extendedMode, high).ok() ||
      c.lowThresholdC > c.highThresholdC)
    return Status::Error(Err::INVALID_CONFIG, "Invalid threshold range or ordering");
  return Status::Ok();
}

uint16_t TMP1x2::encodeConfiguration(const Config& c) {
  return static_cast<uint16_t>(cmd::MASK_RESOLUTION |
      (static_cast<uint16_t>(c.faultQueue) << 11) |
      (static_cast<uint16_t>(c.alertPolarity) << 10) |
      (static_cast<uint16_t>(c.alertMode) << 9) |
      (static_cast<uint16_t>(c.mode) << 8) |
      (static_cast<uint16_t>(c.conversionRate) << 6) |
      (c.extendedMode ? cmd::MASK_EXTENDED_MODE : 0));
}

Status TMP1x2::bind(const Config& config) {
  const Config copy = config; // Support begin(getConfig()).
  const bool oldDirty = _dirty;
  const bool oldFormatRefreshPending = _formatRefreshPending;
  const Status oldDirtyError = _dirtyError;
  unbind();
  _dirty = oldDirty;
  _formatRefreshPending = oldFormatRefreshPending;
  _dirtyError = oldDirtyError;
  Status status = validateConfig(copy);
  if (!status.ok()) return status;
  _config = copy;
  if (_config.offlineThreshold == 0) _config.offlineThreshold = 1;
  uint16_t raw = 0;
  encodeThreshold(copy.lowThresholdC, copy.extendedMode, raw);
  _config.lowThresholdC = decodeThreshold(raw, copy.extendedMode);
  encodeThreshold(copy.highThresholdC, copy.extendedMode, raw);
  _config.highThresholdC = decodeThreshold(raw, copy.extendedMode);
  _bound = true;
  return Status::Ok();
}

void TMP1x2::unbind() {
  _config = Config{};
  _bound = false;
  _initialized = false;
  _state = DriverState::UNINIT;
  _dirty = false;
  _formatRefreshPending = false;
  _dirtyError = Status::Ok();
  _clockSeen = false;
  _tickMs = 0;
  clearConversion();
  _hasSample = false;
  _lastSample = Sample{};
  _consecutiveFailures = 0;
  _totalFailures = _totalSuccess = _lastOkMs = _lastErrorMs = 0;
  _lastError = Status::Ok();
}

Status TMP1x2::begin(const Config& config) {
  Status status = bind(config);
  if (!status.ok()) return status;
  return recover();
}

void TMP1x2::end() {
  _initialized = false;
  _state = DriverState::UNINIT;
  clearConversion();
  _hasSample = false;
}

Status TMP1x2::shutdown() { return setMode(Mode::SHUTDOWN); }

uint32_t TMP1x2::now() const {
  return _config.nowMs ? _config.nowMs(_config.timeUser) : _tickMs;
}

void TMP1x2::tick(uint32_t nowMs) {
  if (!_config.nowMs) {
    _tickMs = nowMs;
    _clockSeen = true;
    if (_conversionStarted && !_conversionClockKnown) {
      _conversionStartMs = nowMs;
      _conversionClockKnown = true;
    }
  }
  if (_initialized && _conversionStarted && !_conversionReady && !_dirty) {
    bool ready = false;
    (void)isConversionReady(ready);
  }
}

Status TMP1x2::guard(bool clean) const {
  if (!_initialized) return Status::Error(Err::NOT_INITIALIZED, "Call begin() first");
  if (clean && _dirty)
    return Status::Error(Err::INVALID_CONFIG, "Hardware configuration is dirty; call recover()");
  return Status::Ok();
}

Status TMP1x2::track(Status status) {
  if (status.ok()) {
    if (_totalSuccess != UINT32_MAX) ++_totalSuccess;
    _consecutiveFailures = 0;
    _lastOkMs = now();
    if (_initialized) _state = DriverState::READY;
  } else if (healthFailure(status.code)) {
    if (_totalFailures != UINT32_MAX) ++_totalFailures;
    if (_consecutiveFailures != UINT8_MAX) ++_consecutiveFailures;
    _lastError = status;
    _lastErrorMs = now();
    if (_initialized)
      _state = _consecutiveFailures >= _config.offlineThreshold ?
          DriverState::OFFLINE : DriverState::DEGRADED;
  }
  return status;
}

Status TMP1x2::read(uint8_t reg, uint16_t& out, bool tracked) {
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  if (reg > cmd::REG_THIGH) return Status::Error(Err::INVALID_PARAM, "Register must be 0..3");
  uint8_t bytes[2] = {0, 0};
  Status status = terminalTransportStatus(_config.i2cWriteRead(
      _config.i2cAddress, &reg, 1, bytes, 2, _config.i2cTimeoutMs, _config.i2cUser));
  if (tracked) status = track(status);
  if (status.ok()) out = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
  return status;
}

Status TMP1x2::write(uint8_t reg, uint16_t value, bool tracked) {
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  if (reg < cmd::REG_CONFIG || reg > cmd::REG_THIGH)
    return Status::Error(Err::INVALID_PARAM, "Only registers 1..3 are writable");
  const uint8_t bytes[3] = {reg, static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
  Status status = terminalTransportStatus(_config.i2cWrite(
      _config.i2cAddress, bytes, 3, _config.i2cTimeoutMs, _config.i2cUser));
  return tracked ? track(status) : status;
}

void TMP1x2::markDirty(Status status) {
  if (!_dirty || (_dirtyError.ok() && !status.ok())) _dirtyError = status;
  _dirty = true;
  clearConversion();
}

void TMP1x2::clearConversion() {
  _conversionStarted = _conversionReady = _conversionClockKnown = false;
  _conversionStartMs = 0;
}

Status TMP1x2::probe() {
  uint16_t raw = 0;
  Status status = read(cmd::REG_CONFIG, raw, false);
  if (!status.ok()) return status;
  return decodeConfiguration(raw).valid ? Status::Ok() :
      Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits do not match TMP1x2", raw);
}

Status TMP1x2::verify(bool tracked) {
  uint16_t config = 0, low = 0, high = 0;
  Status status = read(cmd::REG_CONFIG, config, tracked);
  if (!status.ok()) return status;
  if (!decodeConfiguration(config).valid ||
      (config & cmd::MASK_WRITABLE_CONFIG) !=
          (encodeConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration readback mismatch", config);
    return status;
  }
  status = read(cmd::REG_TLOW, low, tracked);
  if (!status.ok()) return status;
  status = read(cmd::REG_THIGH, high, tracked);
  if (!status.ok()) return status;
  uint16_t expectedLow = 0, expectedHigh = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, expectedLow);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, expectedHigh);
  if (low != expectedLow || high != expectedHigh) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Threshold readback mismatch");
    return status;
  }
  return Status::Ok();
}

Status TMP1x2::apply() {
  uint16_t previous = 0;
  Status status = read(cmd::REG_CONFIG, previous, true);
  if (!status.ok()) { markDirty(status); return status; }
  if (!decodeConfiguration(previous).valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", previous);
    markDirty(status); return status;
  }
  const bool formatChanges = ((previous & cmd::MASK_EXTENDED_MODE) != 0) != _config.extendedMode;
  const bool refreshFormat = formatChanges || _formatRefreshPending;
  const bool enteringShutdown = !(previous & cmd::MASK_SHUTDOWN) && _config.mode == Mode::SHUTDOWN;
  const bool needsSettling = refreshFormat || enteringShutdown;
  if (needsSettling && !_config.nowMs) {
    status = Status::Error(Err::INVALID_CONFIG, "Changing EM or entering shutdown requires nowMs callback");
    markDirty(status); return status;
  }
  uint16_t low = 0, high = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, low);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, high);
  const uint16_t config = encodeConfiguration(_config);
  markDirty(Status::Ok());
  // Preserve the actual format while an old conversion finishes. TI documents
  // that changing EM mid-conversion can tag old-format payload with the new EM.
  const uint16_t shutdown = static_cast<uint16_t>(
      (previous & static_cast<uint16_t>(~cmd::MASK_OS)) | cmd::MASK_SHUTDOWN);
  // Same-format continuous updates stay continuous. Threshold programming is
  // not atomic; applications must disregard ALERT during configuration changes.
  // If settling fails after SD reached hardware, a retry cannot infer whether
  // its in-flight conversion finished merely by reading SD back as one.
  if (needsSettling) _formatRefreshPending = true;
  status = write(cmd::REG_CONFIG, needsSettling ? shutdown : config, true);
  if (status.ok() && needsSettling) status = waitConversionInterval();
  if (status.ok() && refreshFormat) {
    // Preserve uncertainty across partial failures: readback of EM alone does
    // not prove the payload has completed a conversion in that format.
    _formatRefreshPending = true;
    status = write(cmd::REG_CONFIG, config | cmd::MASK_SHUTDOWN, true);
  }
  if (status.ok()) status = write(cmd::REG_TLOW, low, true);
  if (status.ok()) status = write(cmd::REG_THIGH, high, true);
  // Replace the stale payload as well as its format marker before exposing it.
  if (status.ok() && refreshFormat)
    status = write(cmd::REG_CONFIG, config | cmd::MASK_SHUTDOWN | cmd::MASK_OS, true);
  if (status.ok() && refreshFormat) status = waitConversionInterval();
  if (status.ok() && refreshFormat) {
    uint16_t completed = 0;
    status = read(cmd::REG_CONFIG, completed, true);
    if (status.ok() && !(completed & cmd::MASK_OS))
      status = Status::Error(Err::TIMEOUT, "New-format conversion did not finish");
  }
  if (status.ok()) status = write(cmd::REG_CONFIG, config, true);
  if (status.ok()) status = verify(true);
  if (!status.ok()) { markDirty(status); return status; }
  _dirty = false;
  _formatRefreshPending = false;
  _dirtyError = Status::Ok();
  _hasSample = false;
  return Status::Ok();
}

Status TMP1x2::waitConversionInterval() {
  const uint32_t started = now();
  uint32_t previous = started;
  uint32_t unchanged = 0;
  // One extra millisecond covers truncation of the caller's millisecond clock.
  while (static_cast<uint32_t>(now() - started) < cmd::CONVERSION_TIME_MAX_MS + 1u) {
    const uint32_t current = now();
    if (current == previous) {
      if (++unchanged >= 1000000u)
        return Status::Error(Err::INVALID_CONFIG, "nowMs clock did not advance");
    } else { previous = current; unchanged = 0; }
    if (_config.cooperativeYield) _config.cooperativeYield(_config.timeUser);
  }
  return Status::Ok();
}

Status TMP1x2::recover() {
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  uint16_t raw = 0;
  Status status = read(cmd::REG_CONFIG, raw, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(raw).valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", raw);
    markDirty(status);
    return status;
  }
  status = apply();
  if (!status.ok()) return status;
  _initialized = true;
  _state = DriverState::READY;
  return Status::Ok();
}

Status TMP1x2::update(const Config& config) {
  Status status = guard();
  if (!status.ok()) return status;
  if (_conversionStarted) return Status::Error(Err::BUSY, "One-shot conversion pending");
  status = validateConfig(config);
  if (!status.ok()) return Status::Error(Err::INVALID_PARAM, status.msg, status.detail);
  _config = config;
  uint16_t raw = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, raw);
  _config.lowThresholdC = decodeThreshold(raw, _config.extendedMode);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, raw);
  _config.highThresholdC = decodeThreshold(raw, _config.extendedMode);
  return apply();
}

Status TMP1x2::setMode(Mode value) {
  Config c = _config; c.mode = value; return update(c);
}
Status TMP1x2::setConversionRate(ConversionRate value) {
  Config c = _config; c.conversionRate = value; return update(c);
}
Status TMP1x2::setExtendedMode(bool value) {
  Config c = _config; c.extendedMode = value; return update(c);
}
Status TMP1x2::setAlertMode(AlertMode value) {
  Config c = _config; c.alertMode = value; return update(c);
}
Status TMP1x2::setAlertPolarity(AlertPolarity value) {
  Config c = _config; c.alertPolarity = value; return update(c);
}
Status TMP1x2::setFaultQueue(FaultQueue value) {
  Config c = _config; c.faultQueue = value; return update(c);
}
Status TMP1x2::setThresholds(float lowC, float highC) {
  Config c = _config; c.lowThresholdC = lowC; c.highThresholdC = highC; return update(c);
}

Status TMP1x2::readConfiguration(ConfigurationInfo& out) {
  uint16_t raw = 0;
  Status status = readRegister(cmd::REG_CONFIG, raw);
  if (!status.ok()) return status;
  const ConfigurationInfo info = decodeConfiguration(raw);
  if (!info.valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", raw);
    markDirty(status);
    return status;
  }
  if ((raw & cmd::MASK_WRITABLE_CONFIG) !=
      (encodeConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG))
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed configuration differs from desired settings", raw));
  out = info;
  return Status::Ok();
}

Status TMP1x2::readThresholds(float& lowC, float& highC) {
  Status status = guard(true);
  if (!status.ok()) return status;
  uint16_t config = 0, low = 0, high = 0;
  status = read(cmd::REG_CONFIG, config, true);
  if (status.ok()) status = read(cmd::REG_TLOW, low, true);
  if (status.ok()) status = read(cmd::REG_THIGH, high, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(config).valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", config);
    markDirty(status); return status;
  }
  if ((config & cmd::MASK_WRITABLE_CONFIG) !=
      (encodeConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG))
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed configuration differs from desired settings", config));
  const bool extended = (config & cmd::MASK_EXTENDED_MODE) != 0;
  uint16_t expectedLow = 0, expectedHigh = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, expectedLow);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, expectedHigh);
  if (low != expectedLow || high != expectedHigh)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed thresholds differ from desired settings"));
  lowC = decodeThreshold(low, extended);
  highC = decodeThreshold(high, extended);
  return Status::Ok();
}

Status TMP1x2::verifyConfiguration() {
  Status status = guard();
  if (!status.ok()) return status;
  status = verify(true);
  if (!status.ok()) markDirty(status);
  // Dirty is only cleared by a complete reapply, never by a partial observation.
  return status;
}

Status TMP1x2::readAlertPin(bool& active) const {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (!_config.gpioRead || _config.alertPin < 0)
    return Status::Error(Err::INVALID_CONFIG, "No ALERT GPIO hook configured");
  active = _config.gpioRead(_config.alertPin, _config.gpioUser) ==
           (_config.alertPolarity == AlertPolarity::ACTIVE_HIGH);
  return Status::Ok();
}

Status TMP1x2::readSample(Sample& out) {
  Status status = guard(true);
  if (!status.ok()) return status;
  uint16_t raw = 0;
  status = read(cmd::REG_TEMPERATURE, raw, true);
  if (!status.ok()) return status;
  Sample sample;
  status = decodeTemperature(raw, sample);
  if (!status.ok()) { markDirty(status); return status; }
  if (sample.extendedMode != _config.extendedMode) return notReady();
  sample.timestampMs = now();
  _lastSample = sample;
  _hasSample = true;
  out = sample;
  return Status::Ok();
}

Status TMP1x2::readTemperature(float& out) {
  Sample sample;
  const Status status = readSample(sample);
  if (status.ok()) out = sample.celsius;
  return status;
}

Status TMP1x2::startOneShot() {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (_conversionStarted) return Status::Error(Err::BUSY, "One-shot conversion pending");
  if (_config.mode != Mode::SHUTDOWN)
    return Status::Error(Err::INVALID_CONFIG, "One-shot requires SHUTDOWN mode");
  uint16_t current = 0;
  status = read(cmd::REG_CONFIG, current, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(current).valid ||
      (current & cmd::MASK_WRITABLE_CONFIG) !=
          (encodeConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", current);
    markDirty(status); return status;
  }
  if (!(current & cmd::MASK_OS))
    return Status::Error(Err::BUSY, "Previous shutdown conversion is still active");
  status = write(cmd::REG_CONFIG, encodeConfiguration(_config) | cmd::MASK_OS, true);
  if (!status.ok()) { markDirty(status); return status; }
  _conversionStarted = true;
  _conversionReady = false;
  _conversionClockKnown = clockKnown();
  _conversionStartMs = now();
  return Status::Ok();
}

Status TMP1x2::isConversionReady(bool& ready) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (!_conversionStarted) { ready = false; return Status::Ok(); }
  if (_conversionReady) { ready = true; return Status::Ok(); }
  if (_conversionClockKnown && static_cast<uint32_t>(now() - _conversionStartMs) <
      cmd::CONVERSION_TIME_MAX_MS) { ready = false; return Status::Ok(); }
  uint16_t raw = 0;
  status = read(cmd::REG_CONFIG, raw, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(raw).valid ||
      (raw & cmd::MASK_WRITABLE_CONFIG) !=
          (encodeConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", raw);
    markDirty(status); return status;
  }
  _conversionReady = (raw & cmd::MASK_OS) != 0;
  ready = _conversionReady;
  return Status::Ok();
}

Status TMP1x2::tryRead(Sample& out) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (_config.mode == Mode::CONTINUOUS) return readSample(out);
  if (!_conversionStarted) return notReady();
  bool ready = false;
  status = isConversionReady(ready);
  if (!status.ok()) return status;
  if (!ready) return notReady();
  status = readSample(out);
  if (status.ok()) clearConversion();
  return status;
}

Status TMP1x2::readBlocking(Sample& out, uint32_t timeoutMs) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (!_config.nowMs)
    return Status::Error(Err::INVALID_CONFIG, "Blocking read requires nowMs callback");
  if (timeoutMs == 0 || timeoutMs > 0x7FFFFFFFu)
    return Status::Error(Err::INVALID_PARAM, "Timeout must be 1..INT32_MAX ms");
  if (_config.mode == Mode::CONTINUOUS) return readSample(out);
  const uint32_t started = now();
  if (!_conversionStarted) {
    status = startOneShot();
    if (!status.ok()) return status;
  }
  uint32_t previous = started;
  uint32_t unchanged = 0;
  for (;;) {
    const uint32_t current = now();
    if (static_cast<uint32_t>(current - started) >= timeoutMs)
      return Status::Error(Err::TIMEOUT, "One-shot polling deadline expired");
    status = tryRead(out);
    if (!status.is(Err::MEASUREMENT_NOT_READY)) return status;
    if (current == previous) {
      if (++unchanged >= 1000000u)
        return Status::Error(Err::INVALID_CONFIG, "nowMs clock did not advance");
    } else { previous = current; unchanged = 0; }
    if (_config.cooperativeYield) _config.cooperativeYield(_config.timeUser);
  }
}

Status TMP1x2::readRegister(uint8_t reg, uint16_t& out) {
  Status status = guard();
  return status.ok() ? read(reg, out, true) : status;
}

Status TMP1x2::writeRegister(uint8_t reg, uint16_t value) {
  Status status = guard();
  if (!status.ok()) return status;
  if (reg < cmd::REG_CONFIG || reg > cmd::REG_THIGH)
    return Status::Error(Err::INVALID_PARAM, "Only registers 1..3 are writable");
  status = write(reg, value, true);
  if (reg == cmd::REG_CONFIG) _formatRefreshPending = true;
  markDirty(status);
  return status;
}

Status TMP1x2::readRegisterRaw(uint8_t reg, uint16_t& out) { return read(reg, out, false); }
Status TMP1x2::writeRegisterRaw(uint8_t reg, uint16_t value) {
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  if (reg < cmd::REG_CONFIG || reg > cmd::REG_THIGH)
    return Status::Error(Err::INVALID_PARAM, "Only registers 1..3 are writable");
  Status status = write(reg, value, false);
  if (reg == cmd::REG_CONFIG) _formatRefreshPending = true;
  markDirty(status);
  return status;
}

Status TMP1x2::decodeTemperature(uint16_t raw, Sample& out) {
  const bool extended = (raw & cmd::MASK_TEMP_EXTENDED) != 0;
  if ((raw & (extended ? 0x0006u : 0x000Eu)) != 0)
    return Status::Error(Err::CONFIG_MISMATCH, "Temperature reserved bits are nonzero", raw);
  Sample sample;
  sample.raw = raw;
  sample.extendedMode = extended;
  sample.counts = signedCounts(raw, extended);
  sample.celsius = static_cast<float>(sample.counts) * 0.0625f;
  out = sample;
  return Status::Ok();
}

Status TMP1x2::encodeThreshold(float celsius, bool extended, uint16_t& out) {
  const float minimum = extended ? -256.0f : -128.0f;
  const float maximum = extended ? 255.9375f : 127.9375f;
  if (!std::isfinite(celsius) || celsius < minimum || celsius > maximum)
    return Status::Error(Err::INVALID_PARAM, "Threshold outside register range");
  const int32_t counts = static_cast<int32_t>(std::round(celsius * 16.0f));
  const uint16_t bits = static_cast<uint16_t>(counts) & (extended ? 0x1FFFu : 0x0FFFu);
  out = static_cast<uint16_t>(bits << (extended ? 3 : 4));
  return Status::Ok();
}

float TMP1x2::decodeThreshold(uint16_t raw, bool extended) {
  return static_cast<float>(signedCounts(raw, extended)) * 0.0625f;
}

ConfigurationInfo TMP1x2::decodeConfiguration(uint16_t raw) {
  ConfigurationInfo info;
  info.raw = raw;
  info.oneShotReady = (raw & cmd::MASK_OS) != 0;
  info.extendedMode = (raw & cmd::MASK_EXTENDED_MODE) != 0;
  info.alert = (raw & cmd::MASK_ALERT) != 0;
  info.valid = (raw & cmd::MASK_RESOLUTION) == cmd::MASK_RESOLUTION &&
               (raw & cmd::MASK_RESERVED) == 0;
  info.mode = static_cast<Mode>((raw >> 8) & 1);
  info.conversionRate = static_cast<ConversionRate>((raw >> 6) & 3);
  info.alertMode = static_cast<AlertMode>((raw >> 9) & 1);
  info.alertPolarity = static_cast<AlertPolarity>((raw >> 10) & 1);
  info.faultQueue = static_cast<FaultQueue>((raw >> 11) & 3);
  info.alertActive = info.alert == (info.alertPolarity == AlertPolarity::ACTIVE_HIGH);
  return info;
}

uint32_t TMP1x2::conversionPeriodMs(ConversionRate rate) {
  switch (rate) {
    case ConversionRate::HZ_0_25: return 4000;
    case ConversionRate::HZ_1: return 1000;
    case ConversionRate::HZ_4: return 250;
    case ConversionRate::HZ_8: return 125;
  }
  return 0;
}

SettingsSnapshot TMP1x2::getSettingsSnapshot() const {
  SettingsSnapshot snapshot;
  snapshot.bound = _bound;
  snapshot.initialized = _initialized;
  snapshot.state = _state;
  snapshot.config = _config;
  snapshot.hardwareConfigDirty = _dirty;
  snapshot.hardwareConfigDirtyError = _dirtyError;
  snapshot.conversionStarted = _conversionStarted;
  snapshot.conversionReady = _conversionReady;
  snapshot.hasSample = _hasSample;
  snapshot.lastSample = _lastSample;
  return snapshot;
}

uint32_t TMP1x2::sampleAgeMs() const {
  return _hasSample ? static_cast<uint32_t>(now() - _lastSample.timestampMs) : UINT32_MAX;
}
} // namespace TMP1x2
