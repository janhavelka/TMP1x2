/// @file TMP1x2Configuration.cpp
/// @brief Configuration validation, verified replay, and live diagnostics.
#include "TMP1x2/TMP1x2.h"

namespace TMP1x2 {

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
  uint16_t expectedLow = 0, expectedHigh = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, expectedLow);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, expectedHigh);
  status = read(cmd::REG_TLOW, low, tracked);
  if (!status.ok()) return status;
  // Keep an already observed mismatch even if the following transfer fails.
  // Transport health and the cause of lost configuration trust are separate.
  if (low != expectedLow)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Low threshold readback mismatch", low));
  status = read(cmd::REG_THIGH, high, tracked);
  if (!status.ok()) return status;
  if (low != expectedLow || high != expectedHigh) {
    status = Status::Error(Err::CONFIG_MISMATCH, "Threshold readback mismatch",
                           low != expectedLow ? low : high);
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

Status TMP1x2::update(const Config& config) {
  Status status = guard();
  if (!status.ok()) return status;
  if (_conversionStarted) return Status::Error(Err::BUSY, "One-shot conversion pending");
  status = validateConfig(config);
  if (!status.ok()) return Status::Error(Err::INVALID_PARAM, status.msg, status.detail);
  // Adopting another format must not erase the old profile's payload evidence.
  // Hardware may already expose the requested EM marker while TEMP still holds
  // the previous format. The observed marker alone cannot establish freshness.
  if (config.extendedMode != _config.extendedMode) _formatRefreshPending = true;
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
  uint16_t expectedLow = 0, expectedHigh = 0;
  encodeThreshold(_config.lowThresholdC, _config.extendedMode, expectedLow);
  encodeThreshold(_config.highThresholdC, _config.extendedMode, expectedHigh);
  status = read(cmd::REG_TLOW, low, true);
  if (!status.ok()) return status;
  // Preserve each observation before another callback can fail. Outputs remain
  // transactional, but already-observed configuration uncertainty must not be.
  if (low != expectedLow)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed low threshold differs from desired settings", low));
  status = read(cmd::REG_THIGH, high, true);
  if (!status.ok()) return status;
  if (high != expectedHigh)
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Observed high threshold differs from desired settings", high));
  const bool extended = (config & cmd::MASK_EXTENDED_MODE) != 0;
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
} // namespace TMP1x2
