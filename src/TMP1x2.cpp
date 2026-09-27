/// @file TMP1x2.cpp
/// @brief Lifecycle, transport access, passive health, and cached settings.
#include "TMP1x2/TMP1x2.h"

namespace TMP1x2 {
namespace {
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
} // namespace

Status TMP1x2::bind(const Config& config) {
  if (jobLocked()) return Status::Error(Err::BUSY, "Consume or cancel the owner operation first");
  if (_conversionStarted) return Status::Error(Err::BUSY, "Consume or end the manual one-shot first");
  const Config copy = config; // Support begin(getConfig()).
  const bool oldDirty = _dirty;
  const bool oldFormatRefreshPending = _formatRefreshPending;
  const bool changingFormat = _bound && copy.extendedMode != _config.extendedMode;
  const Status oldDirtyError = _dirtyError;
  unbind();
  _dirty = oldDirty;
  _formatRefreshPending = oldFormatRefreshPending;
  _dirtyError = oldDirtyError;
  Status status = validateConfig(copy);
  if (!status.ok()) return status;
  // Rebinding must not erase knowledge of the previously selected TEMP format.
  // The newly selected EM marker can precede a conversion in that format.
  if (changingFormat) _formatRefreshPending = true;
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
  _jobActive = _jobResultPending = _jobEffect = _insidePoll = false;
  _jobToken = 0;
  _jobKind = OperationKind::NONE;
  _jobPhase = JobPhase::IDLE;
  _jobStatus = Status::Ok();
  _jobResult = OperationResult{};
  _jobDesired = Config{};
  _jobSample = Sample{};
  _jobStartedMs = _jobTimeoutMs = _jobWaitStarted = _callbackTimeoutMs = 0;
  _jobYieldPoll = _jobWaitNeedsAnchor = false;
  _apply = ApplyState{};
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
  _lastOkTimeValid = _lastErrorTimeValid = false;
  _lastError = Status::Ok();
}

Status TMP1x2::begin(const Config& config) {
  Status status = bind(config);
  if (!status.ok()) return status;
  return recover();
}

void TMP1x2::end() {
  if (_jobActive) (void)cancel();
  if (_conversionStarted) {
    _formatRefreshPending = true;
    markDirty(Status::Error(Err::CANCELLED, "One-shot tracking ended before consumption"));
  }
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
  if (jobLocked()) return;
  if (_initialized && _conversionStarted && !_conversionReady && !_dirty) {
    bool ready = false;
    (void)isConversionReady(ready);
  }
}

Status TMP1x2::guard(bool clean) const {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
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
    _lastOkTimeValid = clockKnown();
    if (_initialized) _state = DriverState::READY;
  } else if (healthFailure(status.code)) {
    if (_totalFailures != UINT32_MAX) ++_totalFailures;
    if (_consecutiveFailures != UINT8_MAX) ++_consecutiveFailures;
    _lastError = status;
    _lastErrorMs = now();
    _lastErrorTimeValid = clockKnown();
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
      _config.i2cAddress, &reg, 1, bytes, 2,
      _insidePoll ? _callbackTimeoutMs : _config.i2cTimeoutMs, _config.i2cUser));
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
      _config.i2cAddress, bytes, 3,
      _insidePoll ? _callbackTimeoutMs : _config.i2cTimeoutMs, _config.i2cUser));
  return tracked ? track(status) : status;
}

void TMP1x2::markDirty(Status status) {
  if (!_dirty || (_dirtyError.ok() && !status.ok())) _dirtyError = status;
  _dirty = true;
  clearConversion();
}

void TMP1x2::markConfigurationDirty(Status status, uint16_t observed) {
  // A live EM mismatch is evidence about the payload, not just CONFIG. Keep
  // that evidence if a later setter adopts the observed EM or hardware changes
  // back before recovery; neither proves TEMP completed in the current format.
  if (((observed & cmd::MASK_EXTENDED_MODE) != 0) != _config.extendedMode)
    _formatRefreshPending = true;
  markDirty(status);
}

Status TMP1x2::probe() {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  uint16_t raw = 0;
  Status status = read(cmd::REG_CONFIG, raw, false);
  if (!status.ok()) return status;
  return decodeConfiguration(raw).valid ? Status::Ok() :
      Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits do not match TMP1x2", raw);
}

Status TMP1x2::invalidateDeviceState() {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  _formatRefreshPending = true;
  markDirty(Status::Error(Err::CONFIG_MISMATCH, "External hardware state change requires recovery"));
  _hasSample = false;
  return Status::Ok();
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

Status TMP1x2::readRegisterRaw(uint8_t reg, uint16_t& out) {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  return read(reg, out, false);
}

Status TMP1x2::writeRegisterRaw(uint8_t reg, uint16_t value) {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  if (reg < cmd::REG_CONFIG || reg > cmd::REG_THIGH)
    return Status::Error(Err::INVALID_PARAM, "Only registers 1..3 are writable");
  Status status = write(reg, value, false);
  if (reg == cmd::REG_CONFIG) _formatRefreshPending = true;
  markDirty(status);
  return status;
}

HealthSnapshot TMP1x2::healthSnapshot() const {
  HealthSnapshot snapshot;
  snapshot.state = _state;
  snapshot.initialized = _initialized;
  snapshot.consecutiveFailures = _consecutiveFailures;
  snapshot.totalFailures = _totalFailures;
  snapshot.totalSuccess = _totalSuccess;
  snapshot.lastOkMs = _lastOkMs;
  snapshot.lastErrorMs = _lastErrorMs;
  snapshot.lastOkTimeValid = _lastOkTimeValid;
  snapshot.lastErrorTimeValid = _lastErrorTimeValid;
  snapshot.lastError = _lastError;
  return snapshot;
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
} // namespace TMP1x2
