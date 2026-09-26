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
  if (!isValidAddress(c.model, c.i2cAddress))
    return Status::Error(Err::INVALID_CONFIG, "Address is not supported by selected model");
  if (c.i2cTimeoutMs == 0 || c.i2cTimeoutMs > 0x7FFFFFFFu)
    return Status::Error(Err::INVALID_CONFIG, "I2C timeout must be 1..INT32_MAX ms");
  if (c.alertPin >= 0 && !hasAlertOutput(c.model))
    return Status::Error(Err::INVALID_CONFIG, "Selected model has no ALERT output");
  if (c.alertPin < -1 || (c.alertPin >= 0 && !c.gpioRead))
    return Status::Error(Err::INVALID_CONFIG, "Configured ALERT pin requires GPIO callback");
  return validateSettings(c);
}

Status TMP1x2::validateSettings(const Config& c) {
  if (static_cast<uint8_t>(c.mode) > 1 || static_cast<uint8_t>(c.conversionRate) > 3 ||
      static_cast<uint8_t>(c.alertMode) > 1 || static_cast<uint8_t>(c.alertPolarity) > 1 ||
      static_cast<uint8_t>(c.faultQueue) > 3)
    return Status::Error(Err::INVALID_CONFIG, "Invalid configuration enum");
  uint16_t low = 0, high = 0;
  if (!encodeThreshold(c.lowThresholdC, c.extendedMode, low).ok() ||
      !encodeThreshold(c.highThresholdC, c.extendedMode, high).ok() ||
      c.lowThresholdC > c.highThresholdC)
    return Status::Error(Err::INVALID_CONFIG, "Invalid threshold range or ordering");
  return Status::Ok();
}

uint16_t TMP1x2::packConfiguration(const Config& c) {
  return static_cast<uint16_t>(cmd::MASK_RESOLUTION |
      (static_cast<uint16_t>(c.faultQueue) << 11) |
      (static_cast<uint16_t>(c.alertPolarity) << 10) |
      (static_cast<uint16_t>(c.alertMode) << 9) |
      (static_cast<uint16_t>(c.mode) << 8) |
      (static_cast<uint16_t>(c.conversionRate) << 6) |
      (c.extendedMode ? cmd::MASK_EXTENDED_MODE : 0));
}

Config TMP1x2::defaultConfig(Model model) {
  Config config;
  config.model = model;
  config.i2cAddress = modelAddressMin(model);
  return config;
}

Status TMP1x2::encodeConfiguration(const Config& config, uint16_t& out) {
  const Status status = validateSettings(config);
  if (!status.ok()) return status;
  out = packConfiguration(config);
  return Status::Ok();
}

Status TMP1x2::expectedConfigurationRegister(const Config& config, uint8_t reg,
                                            uint16_t& value, uint16_t& mask) {
  if (reg < cmd::REG_CONFIG || reg > cmd::REG_THIGH)
    return Status::Error(Err::INVALID_PARAM, "Register is not persistent configuration");
  const Status status = validateSettings(config);
  if (!status.ok()) return status;
  uint16_t expected = 0;
  uint16_t comparisonMask = 0xFFFFU;
  if (reg == cmd::REG_CONFIG) {
    expected = packConfiguration(config);
    comparisonMask = cmd::MASK_WRITABLE_CONFIG;
  } else {
    (void)encodeThreshold(reg == cmd::REG_TLOW ? config.lowThresholdC : config.highThresholdC,
                          config.extendedMode, expected);
  }
  value = expected;
  mask = comparisonMask;
  return Status::Ok();
}

Status TMP1x2::bind(const Config& config) {
  if (jobLocked()) return Status::Error(Err::BUSY, "Consume or cancel the owner operation first");
  if (_conversionStarted) return Status::Error(Err::BUSY, "Consume or end the manual one-shot first");
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

void TMP1x2::clearConversion() {
  _conversionStarted = _conversionReady = _conversionClockKnown = false;
  _conversionStartMs = 0;
}

Status TMP1x2::probe() {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
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
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration readback mismatch", config);
    markConfigurationDirty(status, config);
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

void TMP1x2::beginApply(const Config& desired, bool owner) {
  _apply = ApplyState{};
  _apply.phase = ApplyPhase::OBSERVE;
  _apply.desired = desired;
  _apply.owner = owner;
  _apply.config = packConfiguration(desired);
  (void)encodeThreshold(desired.lowThresholdC, desired.extendedMode, _apply.low);
  (void)encodeThreshold(desired.highThresholdC, desired.extendedMode, _apply.high);
}

bool TMP1x2::applyWaiting() const {
  return _apply.phase == ApplyPhase::WAIT_OLD || _apply.phase == ApplyPhase::WAIT_NEW;
}

void TMP1x2::startApplyWait(ApplyPhase phase) {
  _apply.phase = phase;
  _apply.waitStarted = now();
  _apply.waitNeedsAnchor = _apply.owner && !_config.nowMs;
  // External-time callers cannot observe this callback's completion inside the
  // current poll. Anchor their settling interval on the next owner poll.
  _jobYieldPoll = _apply.owner;
}

Status TMP1x2::stepApply(bool& transferred) {
  transferred = false;
  const Status pending = Status::Error(Err::IN_PROGRESS, "Configuration in progress");
  Status status;
  uint16_t raw = 0;
  switch (_apply.phase) {
    case ApplyPhase::OBSERVE:
      transferred = true;
      status = read(cmd::REG_CONFIG, raw, true);
      if (!status.ok()) return status;
      _apply.previous = raw;
      if (!decodeConfiguration(raw).valid) {
        status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", raw);
        markConfigurationDirty(status, raw);
        return status;
      }
      // Compare against the existing desired profile before a staged target is
      // adopted; cancellation before a write must not manufacture an EM change.
      if (((raw & cmd::MASK_EXTENDED_MODE) != 0) != _config.extendedMode)
        _formatRefreshPending = true;
      if (_apply.owner && _initialized && (raw & cmd::MASK_WRITABLE_CONFIG) !=
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG))
        markConfigurationDirty(Status::Error(Err::CONFIG_MISMATCH,
            "Observed configuration differs from desired settings", raw), raw);
      _apply.refresh = _formatRefreshPending ||
          (((raw & cmd::MASK_EXTENDED_MODE) != 0) != _apply.desired.extendedMode);
      _apply.settle = _apply.refresh ||
          (!(raw & cmd::MASK_SHUTDOWN) && _apply.desired.mode == Mode::SHUTDOWN);
      if (_apply.settle && !_apply.owner && !_config.nowMs)
        return Status::Error(Err::INVALID_CONFIG, "Changing EM or entering shutdown requires nowMs callback");
      _apply.phase = ApplyPhase::FIRST_WRITE;
      return pending;
    case ApplyPhase::FIRST_WRITE:
      // Commit the validated target before an ambiguous write can reach silicon.
      _config = _apply.desired;
      _apply.committed = true;
      if (_apply.owner) _jobEffect = true;
      markDirty(Status::Ok());
      if (_apply.settle) _formatRefreshPending = true;
      transferred = true;
      raw = _apply.settle ? static_cast<uint16_t>(
          (_apply.previous & static_cast<uint16_t>(~cmd::MASK_OS)) | cmd::MASK_SHUTDOWN) : _apply.config;
      status = write(cmd::REG_CONFIG, raw, true);
      if (!status.ok()) return status;
      if (_apply.settle) startApplyWait(ApplyPhase::WAIT_OLD);
      else _apply.phase = ApplyPhase::WRITE_LOW;
      return pending;
    case ApplyPhase::WAIT_OLD:
    case ApplyPhase::WAIT_NEW:
      if (_apply.waitNeedsAnchor) {
        _apply.waitStarted = now();
        _apply.waitNeedsAnchor = false;
        return pending;
      }
      if (static_cast<uint32_t>(now() - _apply.waitStarted) < cmd::CONVERSION_TIME_MAX_MS + 1U)
        return pending;
      _apply.phase = _apply.phase == ApplyPhase::WAIT_NEW ? ApplyPhase::CHECK_NEW :
          (_apply.refresh ? ApplyPhase::SET_FORMAT : ApplyPhase::WRITE_LOW);
      return pending;
    case ApplyPhase::SET_FORMAT:
      transferred = true;
      status = write(cmd::REG_CONFIG, _apply.config | cmd::MASK_SHUTDOWN, true);
      if (!status.ok()) return status;
      _apply.phase = ApplyPhase::WRITE_LOW;
      return pending;
    case ApplyPhase::WRITE_LOW:
      transferred = true;
      status = write(cmd::REG_TLOW, _apply.low, true);
      if (!status.ok()) return status;
      _apply.phase = ApplyPhase::WRITE_HIGH;
      return pending;
    case ApplyPhase::WRITE_HIGH:
      transferred = true;
      status = write(cmd::REG_THIGH, _apply.high, true);
      if (!status.ok()) return status;
      _apply.phase = _apply.refresh ? ApplyPhase::TRIGGER : ApplyPhase::RESTORE;
      return pending;
    case ApplyPhase::TRIGGER:
      transferred = true;
      status = write(cmd::REG_CONFIG, _apply.config | cmd::MASK_SHUTDOWN | cmd::MASK_OS, true);
      if (!status.ok()) return status;
      startApplyWait(ApplyPhase::WAIT_NEW);
      return pending;
    case ApplyPhase::CHECK_NEW:
      transferred = true;
      status = read(cmd::REG_CONFIG, raw, true);
      if (!status.ok()) return status;
      if (!decodeConfiguration(raw).valid || (raw & cmd::MASK_WRITABLE_CONFIG) !=
          ((_apply.config | cmd::MASK_SHUTDOWN) & cmd::MASK_WRITABLE_CONFIG)) {
        status = Status::Error(Err::CONFIG_MISMATCH, "New-format configuration changed", raw);
        markConfigurationDirty(status, raw);
        return status;
      }
      if (!(raw & cmd::MASK_OS))
        return Status::Error(Err::TIMEOUT, "New-format conversion did not finish");
      _apply.phase = ApplyPhase::RESTORE;
      return pending;
    case ApplyPhase::RESTORE:
      transferred = true;
      status = write(cmd::REG_CONFIG, _apply.config, true);
      if (!status.ok()) return status;
      _apply.phase = ApplyPhase::VERIFY_CONFIG;
      return pending;
    case ApplyPhase::VERIFY_CONFIG:
      transferred = true;
      status = read(cmd::REG_CONFIG, raw, true);
      if (!status.ok()) return status;
      if (!decodeConfiguration(raw).valid || (raw & cmd::MASK_WRITABLE_CONFIG) !=
          (_apply.config & cmd::MASK_WRITABLE_CONFIG)) {
        status = Status::Error(Err::CONFIG_MISMATCH, "Configuration readback mismatch", raw);
        markConfigurationDirty(status, raw);
        return status;
      }
      _apply.phase = ApplyPhase::VERIFY_LOW;
      return pending;
    case ApplyPhase::VERIFY_LOW:
    case ApplyPhase::VERIFY_HIGH:
      transferred = true;
      status = read(_apply.phase == ApplyPhase::VERIFY_LOW ? cmd::REG_TLOW : cmd::REG_THIGH, raw, true);
      if (!status.ok()) return status;
      if (raw != (_apply.phase == ApplyPhase::VERIFY_LOW ? _apply.low : _apply.high))
        return Status::Error(Err::CONFIG_MISMATCH, "Threshold readback mismatch", raw);
      _apply.phase = _apply.phase == ApplyPhase::VERIFY_LOW ? ApplyPhase::VERIFY_HIGH : ApplyPhase::COMPLETE;
      return pending;
    case ApplyPhase::COMPLETE:
      _dirty = false;
      _formatRefreshPending = false;
      _dirtyError = Status::Ok();
      _hasSample = false;
      _apply.phase = ApplyPhase::IDLE;
      return Status::Ok();
    case ApplyPhase::IDLE:
      return Status::Error(Err::INVALID_CONFIG, "No configuration operation is active");
  }
  return Status::Error(Err::INVALID_CONFIG, "Invalid configuration operation phase");
}

Status TMP1x2::apply() {
  beginApply(_config, false);
  uint32_t previous = now();
  uint32_t unchanged = 0;
  for (;;) {
    bool transferred = false;
    Status status = stepApply(transferred);
    if (!status.inProgress()) {
      if (!status.ok()) markDirty(status);
      return status;
    }
    if (applyWaiting()) {
      const uint32_t current = now();
      if (current == previous) {
        if (++unchanged >= 1000000U) {
          status = Status::Error(Err::INVALID_CONFIG, "nowMs clock did not advance");
          markDirty(status);
          return status;
        }
      } else { previous = current; unchanged = 0; }
      if (_config.cooperativeYield) _config.cooperativeYield(_config.timeUser);
    }
  }
}

Status TMP1x2::recover() {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  if (_conversionStarted) return Status::Error(Err::BUSY, "Consume or end the manual one-shot first");
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  uint16_t raw = 0;
  Status status = read(cmd::REG_CONFIG, raw, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(raw).valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", raw);
    markConfigurationDirty(status, raw);
    return status;
  }
  // Preserve the observation before apply() performs another transaction.
  // A failure there must not erase evidence of a possibly stale TEMP payload.
  if (((raw & cmd::MASK_EXTENDED_MODE) != 0) != _config.extendedMode)
    _formatRefreshPending = true;
  status = apply();
  if (!status.ok()) return status;
  _initialized = true;
  _state = DriverState::READY;
  return Status::Ok();
}

Status TMP1x2::invalidateDeviceState() {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  _formatRefreshPending = true;
  markDirty(Status::Error(Err::CONFIG_MISMATCH, "External hardware state change requires recovery"));
  _hasSample = false;
  return Status::Ok();
}

Status TMP1x2::admit(OperationKind kind, const Config& desired, uint32_t nowMs,
                      uint32_t timeoutMs, OperationToken& token) {
  if (jobLocked()) return Status::Error(Err::BUSY, "Owner operation or result is pending");
  if (!_bound) return Status::Error(Err::NOT_BOUND, "No transport is bound");
  if (timeoutMs == 0 || timeoutMs > 0x7FFFFFFFU)
    return Status::Error(Err::INVALID_PARAM, "Operation timeout must be 1..INT32_MAX ms");
  if (_nextToken == UINT32_MAX)
    return Status::Error(Err::INVALID_CONFIG, "Operation token space exhausted");
  if (_conversionStarted) return Status::Error(Err::BUSY, "Consume the manual one-shot first");
  if (kind == OperationKind::CONFIGURE || kind == OperationKind::SHUTDOWN || kind == OperationKind::READ) {
    Status status = guard(kind == OperationKind::READ);
    if (!status.ok()) return status;
  }
  Status status = validateConfig(desired);
  if (!status.ok()) return Status::Error(Err::INVALID_PARAM, status.msg, status.detail);
  // Profiles never replace a live transport or its time/GPIO ownership.
  if (desired.i2cWrite != _config.i2cWrite || desired.i2cWriteRead != _config.i2cWriteRead ||
      desired.i2cUser != _config.i2cUser || desired.i2cAddress != _config.i2cAddress ||
      desired.i2cTimeoutMs != _config.i2cTimeoutMs || desired.model != _config.model ||
      desired.nowMs != _config.nowMs || desired.cooperativeYield != _config.cooperativeYield ||
      desired.timeUser != _config.timeUser || desired.alertPin != _config.alertPin ||
      desired.gpioRead != _config.gpioRead || desired.gpioUser != _config.gpioUser ||
      (desired.offlineThreshold == 0 ? 1 : desired.offlineThreshold) != _config.offlineThreshold)
    return Status::Error(Err::INVALID_PARAM, "Configure cannot change binding or hooks; use bind()");
  _jobDesired = desired;
  if (_jobDesired.offlineThreshold == 0) _jobDesired.offlineThreshold = 1;
  uint16_t encoded = 0;
  (void)encodeThreshold(desired.lowThresholdC, desired.extendedMode, encoded);
  _jobDesired.lowThresholdC = decodeThreshold(encoded, desired.extendedMode);
  (void)encodeThreshold(desired.highThresholdC, desired.extendedMode, encoded);
  _jobDesired.highThresholdC = decodeThreshold(encoded, desired.extendedMode);
  if (!_config.nowMs) { _tickMs = nowMs; _clockSeen = true; }
  _jobStartedMs = now();
  _jobTimeoutMs = timeoutMs;
  _jobToken = ++_nextToken;
  _jobKind = kind;
  _jobActive = true;
  _jobEffect = false;
  _jobSample = Sample{};
  _jobWaitNeedsAnchor = false;
  _jobStatus = Status::Error(Err::IN_PROGRESS, "Owner operation in progress");
  if (kind == OperationKind::READ) {
    _jobPhase = _config.mode == Mode::CONTINUOUS ? JobPhase::READ_TEMP : JobPhase::READ_CHECK;
  } else {
    _jobPhase = JobPhase::APPLY;
    beginApply(_jobDesired, true);
  }
  token = _jobToken;
  return _jobStatus;
}

Status TMP1x2::startInitialize(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token) {
  return admit(OperationKind::INITIALIZE, _config, nowMs, timeoutMs, token);
}
Status TMP1x2::startConfigure(const Config& desired, uint32_t nowMs, uint32_t timeoutMs, OperationToken& token) {
  return admit(OperationKind::CONFIGURE, desired, nowMs, timeoutMs, token);
}
Status TMP1x2::startRecover(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token) {
  return admit(OperationKind::RECOVER, _config, nowMs, timeoutMs, token);
}
Status TMP1x2::startShutdown(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token) {
  Config desired = _config;
  desired.mode = Mode::SHUTDOWN;
  return admit(OperationKind::SHUTDOWN, desired, nowMs, timeoutMs, token);
}
Status TMP1x2::startRead(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token) {
  return admit(OperationKind::READ, _config, nowMs, timeoutMs, token);
}

bool TMP1x2::jobExpired() const {
  return static_cast<uint32_t>(now() - _jobStartedMs) >= _jobTimeoutMs;
}
uint32_t TMP1x2::jobRemainingMs() const {
  const uint32_t elapsed = static_cast<uint32_t>(now() - _jobStartedMs);
  return elapsed >= _jobTimeoutMs ? 0 : _jobTimeoutMs - elapsed;
}
bool TMP1x2::jobWaiting() const {
  return _jobActive && ((_jobPhase == JobPhase::APPLY && applyWaiting()) || _jobPhase == JobPhase::READ_WAIT);
}
uint32_t TMP1x2::nextJobPollMs() const {
  if (!jobWaiting()) return now();
  const uint32_t current = now();
  const bool applying = _jobPhase == JobPhase::APPLY;
  if (applying ? _apply.waitNeedsAnchor : _jobWaitNeedsAnchor) return current;
  const uint32_t elapsed = static_cast<uint32_t>(current -
      (applying ? _apply.waitStarted : _jobWaitStarted));
  const uint32_t waitRemaining = elapsed >= cmd::CONVERSION_TIME_MAX_MS + 1U ? 0 :
      cmd::CONVERSION_TIME_MAX_MS + 1U - elapsed;
  const uint32_t operationRemaining = jobRemainingMs();
  return current + (waitRemaining < operationRemaining ? waitRemaining : operationRemaining);
}

Status TMP1x2::finishJob(Status status) {
  if (!status.ok()) {
    if (_jobEffect) {
      if (_jobKind == OperationKind::READ) _formatRefreshPending = true;
      markDirty(status);
    }
  } else if (_jobKind == OperationKind::READ) {
    _lastSample = _jobSample;
    _hasSample = true;
    clearConversion();
  } else {
    _initialized = true;
    _state = DriverState::READY;
  }
  _jobStatus = status;
  _jobResult = OperationResult{};
  _jobResult.token = _jobToken;
  _jobResult.kind = _jobKind;
  _jobResult.status = status;
  _jobResult.hasSample = status.ok() && _jobKind == OperationKind::READ;
  if (_jobResult.hasSample) _jobResult.sample = _jobSample;
  _jobResult.hardwareEffectPossible = _jobEffect;
  _jobResult.startedMs = _jobStartedMs;
  _jobResult.completedMs = now();
  _jobActive = false;
  _jobResultPending = true;
  _jobPhase = JobPhase::IDLE;
  return status;
}

Status TMP1x2::stepJob(bool& transferred) {
  transferred = false;
  const Status pending = Status::Error(Err::IN_PROGRESS, "Owner operation in progress");
  Status status;
  uint16_t raw = 0;
  switch (_jobPhase) {
    case JobPhase::APPLY:
      return stepApply(transferred);
    case JobPhase::READ_CHECK:
    case JobPhase::READ_READY:
      transferred = true;
      status = read(cmd::REG_CONFIG, raw, true);
      if (!status.ok()) return status;
      if (!decodeConfiguration(raw).valid || (raw & cmd::MASK_WRITABLE_CONFIG) !=
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
        status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", raw);
        markConfigurationDirty(status, raw);
        return status;
      }
      if (!(raw & cmd::MASK_OS)) {
        if (_jobPhase == JobPhase::READ_CHECK)
          return Status::Error(Err::BUSY, "Previous shutdown conversion is still active");
        _jobYieldPoll = true;
        return pending;
      }
      _jobPhase = _jobPhase == JobPhase::READ_CHECK ? JobPhase::READ_TRIGGER : JobPhase::READ_TEMP;
      return pending;
    case JobPhase::READ_TRIGGER:
      transferred = true;
      _jobEffect = true;
      status = write(cmd::REG_CONFIG, packConfiguration(_config) | cmd::MASK_OS, true);
      if (!status.ok()) return status;
      _conversionStarted = true;
      _conversionReady = false;
      _conversionClockKnown = true;
      _conversionStartMs = now();
      _jobWaitStarted = now();
      _jobWaitNeedsAnchor = !_config.nowMs;
      _jobPhase = JobPhase::READ_WAIT;
      _jobYieldPoll = true;
      return pending;
    case JobPhase::READ_WAIT:
      if (_jobWaitNeedsAnchor) {
        _jobWaitStarted = now();
        _conversionStartMs = _jobWaitStarted;
        _jobWaitNeedsAnchor = false;
        return pending;
      }
      if (static_cast<uint32_t>(now() - _jobWaitStarted) < cmd::CONVERSION_TIME_MAX_MS + 1U)
        return pending;
      _jobPhase = JobPhase::READ_READY;
      return pending;
    case JobPhase::READ_TEMP:
      transferred = true;
      status = read(cmd::REG_TEMPERATURE, raw, true);
      if (!status.ok()) return status;
      status = decodeObservedTemperature(raw, _jobSample);
      if (!status.ok()) return status;
      _jobSample.timestampMs = now();
      _jobSample.timestampValid = clockKnown();
      _jobSample.freshConversion = _config.mode == Mode::SHUTDOWN;
      _jobPhase = JobPhase::COMPLETE;
      return pending;
    case JobPhase::COMPLETE:
      return Status::Ok();
    case JobPhase::IDLE:
      return Status::Error(Err::RESULT_NOT_AVAILABLE, "No owner operation is active");
  }
  return Status::Error(Err::INVALID_CONFIG, "Invalid owner operation phase");
}

PollResult TMP1x2::poll(uint32_t nowMs, uint8_t maxTransfers) {
  PollResult result;
  if (!_config.nowMs) { _tickMs = nowMs; _clockSeen = true; }
  if (!_jobActive) {
    result.done = _jobResultPending;
    result.status = _jobResultPending ? _jobResult.status :
        Status::Error(Err::RESULT_NOT_AVAILABLE, "No owner operation or result is available");
    result.nextPollMs = now();
    return result;
  }
  _jobYieldPoll = false;
  uint32_t callbackBudget = jobRemainingMs();
  for (;;) {
    if (jobExpired()) {
      (void)finishJob(Status::Error(Err::OPERATION_TIMEOUT, "Owner operation deadline expired"));
      break;
    }
    if (maxTransfers == 0) break;
    const bool noTransferPhase = _jobPhase == JobPhase::COMPLETE || _jobPhase == JobPhase::READ_WAIT ||
        (_jobPhase == JobPhase::APPLY && (applyWaiting() || _apply.phase == ApplyPhase::COMPLETE));
    if (!noTransferPhase && (result.transfers >= maxTransfers || callbackBudget == 0)) break;
    const uint32_t remaining = jobRemainingMs();
    if (remaining == 0) {
      (void)finishJob(Status::Error(Err::OPERATION_TIMEOUT, "Owner operation deadline expired"));
      break;
    }
    _callbackTimeoutMs = _config.i2cTimeoutMs < remaining ? _config.i2cTimeoutMs : remaining;
    if (!_config.nowMs && _callbackTimeoutMs > callbackBudget) _callbackTimeoutMs = callbackBudget;
    _insidePoll = true;
    bool transferred = false;
    const Status status = stepJob(transferred);
    _insidePoll = false;
    if (transferred) {
      ++result.transfers;
      if (!_config.nowMs) callbackBudget -= _callbackTimeoutMs;
    }
    // Never publish a completed sample/configuration after a callback overran
    // the owner's deadline. Transport health still records its actual outcome.
    if (jobExpired()) {
      (void)finishJob(Status::Error(Err::OPERATION_TIMEOUT, "Owner operation deadline expired"));
      break;
    }
    if (!status.inProgress()) { (void)finishJob(status); break; }
    if (_jobYieldPoll || (!transferred && jobWaiting())) break;
  }
  result.done = !_jobActive;
  result.status = _jobStatus;
  result.nextPollMs = nextJobPollMs();
  return result;
}

Status TMP1x2::cancel() {
  if (!_jobActive) return Status::Error(_jobResultPending ? Err::BUSY : Err::RESULT_NOT_AVAILABLE,
      "No active owner operation to cancel");
  return finishJob(Status::Error(Err::CANCELLED, "Owner operation cancelled"));
}
Status TMP1x2::takeResult(OperationToken token, OperationResult& out) {
  if (!_jobResultPending) return Status::Error(Err::RESULT_NOT_AVAILABLE, "No terminal result is available");
  if (token != _jobResult.token) return Status::Error(Err::TOKEN_MISMATCH, "Operation token does not match result");
  out = _jobResult;
  _jobResultPending = false;
  return Status::Ok();
}
OperationSnapshot TMP1x2::getOperationSnapshot() const {
  OperationSnapshot snapshot;
  snapshot.active = _jobActive;
  snapshot.resultPending = _jobResultPending;
  snapshot.token = _jobToken;
  snapshot.kind = _jobKind;
  snapshot.status = _jobStatus;
  snapshot.startedMs = _jobStartedMs;
  snapshot.timeoutMs = _jobTimeoutMs;
  snapshot.waiting = jobWaiting();
  snapshot.nextPollMs = nextJobPollMs();
  snapshot.hardwareEffectPossible = _jobEffect;
  snapshot.desiredConfig = _jobDesired;
  return snapshot;
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
    markConfigurationDirty(status, raw);
    return status;
  }
  if ((raw & cmd::MASK_WRITABLE_CONFIG) !=
      (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG))
    markConfigurationDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed configuration differs from desired settings", raw), raw);
  out = info;
  return Status::Ok();
}

Status TMP1x2::readConfiguration(uint16_t& out) {
  ConfigurationInfo info;
  const Status status = readConfiguration(info);
  if (status.ok()) out = info.raw;
  return status;
}

Status TMP1x2::readSnapshot(RegisterSnapshot& out) {
  Status status = guard();
  if (!status.ok()) return status;
  RegisterSnapshot snapshot;
  ConfigurationInfo first;
  status = readConfiguration(first);
  if (!status.ok()) return status;
  status = read(cmd::REG_TEMPERATURE, snapshot.rawTemperature, true);
  if (!status.ok()) return status;
  const uint32_t temperatureAt = now();
  const bool temperatureTimeValid = clockKnown();
  if (((snapshot.rawTemperature & cmd::MASK_TEMP_EXTENDED) != 0) != _config.extendedMode) {
    _formatRefreshPending = true;
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed temperature format differs from desired settings",
                           snapshot.rawTemperature));
  }
  status = decodeTemperature(snapshot.rawTemperature, snapshot.temperature);
  snapshot.temperatureDecoded = status.ok();
  if (!status.ok()) markDirty(status);
  else {
    snapshot.temperature.timestampMs = temperatureAt;
    snapshot.temperature.timestampValid = temperatureTimeValid;
  }
  uint16_t expectedLow = 0, expectedHigh = 0;
  (void)encodeThreshold(_config.lowThresholdC, _config.extendedMode, expectedLow);
  (void)encodeThreshold(_config.highThresholdC, _config.extendedMode, expectedHigh);
  status = read(cmd::REG_TLOW, snapshot.rawLowThreshold, true);
  if (!status.ok()) return status;
  if (snapshot.rawLowThreshold != expectedLow)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed low threshold differs from desired settings"));
  status = read(cmd::REG_THIGH, snapshot.rawHighThreshold, true);
  if (!status.ok()) return status;
  if (snapshot.rawHighThreshold != expectedHigh)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed high threshold differs from desired settings"));
  status = readConfiguration(snapshot.configuration);
  if (!status.ok()) return status;
  if ((first.raw & cmd::MASK_WRITABLE_CONFIG) !=
      (snapshot.configuration.raw & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration changed during register snapshot",
                           snapshot.configuration.raw);
    markConfigurationDirty(status, snapshot.configuration.raw);
    return status;
  }
  const bool extended = snapshot.configuration.extendedMode;
  snapshot.lowThresholdC = decodeThreshold(snapshot.rawLowThreshold, extended);
  snapshot.highThresholdC = decodeThreshold(snapshot.rawHighThreshold, extended);
  snapshot.configurationMatchesDesired = (snapshot.configuration.raw & cmd::MASK_WRITABLE_CONFIG) ==
      (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG);
  snapshot.thresholdsMatchDesired = extended == _config.extendedMode &&
      snapshot.rawLowThreshold == expectedLow && snapshot.rawHighThreshold == expectedHigh;
  snapshot.temperatureTrusted = snapshot.temperatureDecoded && !_dirty && !_formatRefreshPending &&
      snapshot.temperature.extendedMode == snapshot.configuration.extendedMode;
  snapshot.timestampMs = now();
  snapshot.timestampValid = clockKnown();
  out = snapshot;
  return Status::Ok();
}

Status TMP1x2::readThresholds(float& lowC, float& highC) {
  Status status = guard(true);
  if (!status.ok()) return status;
  uint16_t config = 0, low = 0, high = 0;
  status = read(cmd::REG_CONFIG, config, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(config).valid) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Configuration fixed bits invalid", config);
    markConfigurationDirty(status, config); return status;
  }
  if ((config & cmd::MASK_WRITABLE_CONFIG) !=
      (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG))
    markConfigurationDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed configuration differs from desired settings", config), config);
  status = read(cmd::REG_TLOW, low, true);
  if (status.ok()) status = read(cmd::REG_THIGH, high, true);
  if (!status.ok()) return status;
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
  if (!hasAlertOutput(_config.model))
    return Status::Error(Err::INVALID_CONFIG, "Selected model has no ALERT output");
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
  status = decodeObservedTemperature(raw, sample);
  if (!status.ok()) return status;
  sample.timestampMs = now();
  sample.timestampValid = clockKnown();
  _lastSample = sample;
  _hasSample = true;
  out = sample;
  return Status::Ok();
}

Status TMP1x2::decodeObservedTemperature(uint16_t raw, Sample& sample) {
  const bool formatMismatch = ((raw & cmd::MASK_TEMP_EXTENDED) != 0) != _config.extendedMode;
  if (formatMismatch) _formatRefreshPending = true;
  Status status = decodeTemperature(raw, sample);
  if (!status.ok()) { markDirty(status); return status; }
  if (formatMismatch) {
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Temperature format differs from desired settings", raw));
    return notReady();
  }
  return Status::Ok();
}

Status TMP1x2::readTemperature(float& out) {
  Sample sample;
  const Status status = readSample(sample);
  if (status.ok()) out = sample.celsius;
  return status;
}

Status TMP1x2::readTemperatureFahrenheit(float& out) {
  float celsius = 0;
  const Status status = readTemperature(celsius);
  if (status.ok()) out = celsiusToFahrenheit(celsius);
  return status;
}

Status TMP1x2::readTemperatureCounts(int16_t& out) {
  Sample sample;
  const Status status = readSample(sample);
  if (status.ok()) out = sample.counts;
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
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", current);
    markConfigurationDirty(status, current); return status;
  }
  if (!(current & cmd::MASK_OS))
    return Status::Error(Err::BUSY, "Previous shutdown conversion is still active");
  status = write(cmd::REG_CONFIG, packConfiguration(_config) | cmd::MASK_OS, true);
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
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", raw);
    markConfigurationDirty(status, raw); return status;
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
  if (status.ok()) {
    out.freshConversion = true;
    _lastSample.freshConversion = true;
    clearConversion();
  }
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
  const float minimum = minimumThresholdC(extended);
  const float maximum = maximumThresholdC(extended);
  if (!std::isfinite(celsius) || celsius < minimum || celsius > maximum)
    return Status::Error(Err::INVALID_PARAM, "Threshold outside register range");
  const int32_t counts = static_cast<int32_t>(std::round(celsius * 16.0f));
  const uint16_t bits = static_cast<uint16_t>(counts) & (extended ? 0x1FFFu : 0x0FFFu);
  out = static_cast<uint16_t>(bits << (extended ? 3 : 4));
  return Status::Ok();
}

Status TMP1x2::celsiusToCounts(float celsius, bool extended, int16_t& out) {
  uint16_t raw = 0;
  const Status status = encodeThreshold(celsius, extended, raw);
  if (status.ok()) out = signedCounts(raw, extended);
  return status;
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

float TMP1x2::conversionRateHz(ConversionRate rate) {
  switch (rate) {
    case ConversionRate::HZ_0_25: return 0.25f;
    case ConversionRate::HZ_1: return 1.0f;
    case ConversionRate::HZ_4: return 4.0f;
    case ConversionRate::HZ_8: return 8.0f;
  }
  return 0;
}

uint8_t TMP1x2::faultQueueCount(FaultQueue value) {
  switch (value) {
    case FaultQueue::FAULTS_1: return 1;
    case FaultQueue::FAULTS_2: return 2;
    case FaultQueue::FAULTS_4: return 4;
    case FaultQueue::FAULTS_6: return 6;
  }
  return 0;
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

Status TMP1x2::getLastSample(Sample& out) const {
  if (!_hasSample) return notReady();
  out = _lastSample;
  return Status::Ok();
}

Status TMP1x2::getLastSample(Sample& out, uint32_t nowMs, uint32_t maxAgeMs) const {
  if (maxAgeMs > 0x7FFFFFFFU)
    return Status::Error(Err::INVALID_PARAM, "Sample age budget must be 0..INT32_MAX ms");
  if (!_hasSample) return notReady();
  if (_dirty) return Status::Error(Err::INVALID_CONFIG, "Cached sample configuration is no longer trusted");
  if (!sampleFresh(nowMs, maxAgeMs)) return notReady();
  out = _lastSample;
  return Status::Ok();
}

bool TMP1x2::sampleFresh(uint32_t nowMs, uint32_t maxAgeMs) const {
  return _initialized && !_dirty && _hasSample && _lastSample.timestampValid &&
      maxAgeMs <= 0x7FFFFFFFU && static_cast<uint32_t>(nowMs - _lastSample.timestampMs) <= maxAgeMs;
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

uint32_t TMP1x2::sampleAgeMs(uint32_t nowMs) const {
  return _hasSample ? static_cast<uint32_t>(nowMs - _lastSample.timestampMs) : UINT32_MAX;
}
} // namespace TMP1x2
