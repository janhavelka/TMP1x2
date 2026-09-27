/// @file TMP1x2Operations.cpp
/// @brief Caller-driven owner operations, deadlines, and retained results.
#include "TMP1x2/TMP1x2.h"

namespace TMP1x2 {

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
} // namespace TMP1x2
