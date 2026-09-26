/// @file TMP1x2.h
/// @brief Framework-neutral TI TMP102/TMP112 temperature sensor driver.
#pragma once
#include "TMP1x2/CommandTable.h"
#include "TMP1x2/Config.h"
#include "TMP1x2/Status.h"
#include "TMP1x2/Version.h"

namespace TMP1x2 {
enum class DriverState : uint8_t { UNINIT, READY, DEGRADED, OFFLINE };
constexpr const char* driverStateName(DriverState v) {
  return v == DriverState::UNINIT ? "UNINIT" : v == DriverState::READY ? "READY" :
         v == DriverState::DEGRADED ? "DEGRADED" : v == DriverState::OFFLINE ? "OFFLINE" : "UNKNOWN";
}
constexpr const char* toString(DriverState v) { return driverStateName(v); }
struct Sample {
  uint16_t raw = 0;
  int16_t counts = 0; ///< Signed 1/16 C units.
  float celsius = 0;
  bool extendedMode = false;
  uint32_t timestampMs = 0;
};
using OperationToken = uint32_t; ///< Nonzero identity, never reused by an instance.
enum class OperationKind : uint8_t { NONE, INITIALIZE, CONFIGURE, RECOVER, SHUTDOWN, READ };
constexpr const char* toString(OperationKind value) {
  switch (value) {
    case OperationKind::NONE: return "NONE";
    case OperationKind::INITIALIZE: return "INITIALIZE";
    case OperationKind::CONFIGURE: return "CONFIGURE";
    case OperationKind::RECOVER: return "RECOVER";
    case OperationKind::SHUTDOWN: return "SHUTDOWN";
    case OperationKind::READ: return "READ";
  }
  return "UNKNOWN";
}
struct OperationResult {
  OperationToken token = 0;
  OperationKind kind = OperationKind::NONE;
  Status status = Status::Ok();
  bool hasSample = false; ///< True only for a timely successful READ.
  Sample sample{};
  bool hardwareEffectPossible = false; ///< A mutating write was attempted.
  uint32_t startedMs = 0;
  uint32_t completedMs = 0;
};
struct OperationSnapshot {
  bool active = false;
  bool resultPending = false;
  OperationToken token = 0;
  OperationKind kind = OperationKind::NONE;
  Status status = Status::Ok();
  uint32_t startedMs = 0;
  uint32_t timeoutMs = 0;
  bool waiting = false;
  uint32_t nextPollMs = 0;
  bool hardwareEffectPossible = false;
  Config desiredConfig{}; ///< Staged target; committed on the first write attempt.
};
struct PollResult {
  Status status = Status::Ok();
  uint8_t transfers = 0;
  bool done = false;
  uint32_t nextPollMs = 0;
};
struct ConfigurationInfo {
  uint16_t raw = 0;
  bool oneShotReady = false; ///< OS; meaningful for one-shot in shutdown only.
  bool extendedMode = false;
  bool alert = false; ///< Raw AL bit (not an assertion flag).
  bool alertActive = false; ///< AL == POL.
  bool valid = false; ///< Fixed resolution and reserved bit pattern valid.
  Mode mode = Mode::CONTINUOUS;
  ConversionRate conversionRate = ConversionRate::HZ_4;
  AlertMode alertMode = AlertMode::COMPARATOR;
  AlertPolarity alertPolarity = AlertPolarity::ACTIVE_LOW;
  FaultQueue faultQueue = FaultQueue::FAULTS_1;
};
struct SettingsSnapshot {
  bool bound = false;
  bool initialized = false;
  DriverState state = DriverState::UNINIT;
  Config config{}; ///< Desired configuration; callbacks remain application-owned.
  bool hardwareConfigDirty = false;
  Status hardwareConfigDirtyError = Status::Ok();
  bool conversionStarted = false;
  bool conversionReady = false;
  bool hasSample = false;
  Sample lastSample{};
};

/// No allocation, threads, platform headers, GPIO setup, or bus ownership.
/// Serialize all calls externally; no method is ISR-safe or reentrant.
/// Reading sensor registers may acknowledge the interrupt-mode ALERT latch;
/// treat diagnostic reads as potentially destructive to the latched indication.
///
/// Typed setters cache validated desired settings before applying them. A failed
/// write can have reached hardware: desired settings remain available, dirty is
/// latched, and recover() reapplies them. Failed output arguments stay unchanged.
/// Observed EM differences retain a required format refresh even if a missing
/// clock prevented writes; changing desired EM again does not clear that evidence.
/// Normal calls require begin() or completed owner initialization/recovery.
/// OFFLINE is passive health telemetry:
/// calls still access the bus, and successful tracked I/O restores READY.
/// Raw calls and probe() require bind() only and never change health. Raw writes
/// conservatively dirty managed state. Raw CONFIG writes also require a fresh
/// conversion during recovery, so synchronous recovery requires nowMs. Owner
/// recovery can use caller-supplied poll times. No general-call
/// reset is issued.
class TMP1x2 {
public:
  TMP1x2() = default;
  TMP1x2(const TMP1x2&) = delete;
  TMP1x2& operator=(const TMP1x2&) = delete;
  TMP1x2(TMP1x2&&) = delete;
  TMP1x2& operator=(TMP1x2&&) = delete;

  /// Validate/cache transport without I2C; clears previous binding/runtime.
  /// Existing dirty evidence persists until successful full reapply or unbind().
  /// An active operation or unconsumed result returns BUSY without changing state.
  /// A pending manual one-shot also requires consumption or end() before rebind.
  Status bind(const Config& config);
  /// Forget binding, pending results and dirty evidence without I2C. Operation
  /// token identity is lifetime-scoped and is not reset by this explicit release.
  void unbind();
  /// Apply and verify all configuration. An actual EM-format change requires
  /// nowMs; it settles the old conversion then obtains a fresh conversion in
  /// the new format before returning (two conservative 35-ms waits, each with
  /// a 1-ms clock-quantization margin). Entering
  /// shutdown from continuous also requires nowMs to settle the old conversion
  /// (35 ms plus 1-ms margin). Same-format continuous initialization needs no clock callback.
  Status begin(const Config& config);
  /// Local deinitialization only; retains binding/config and never touches I2C.
  /// Cancels an owner operation and retains its terminal result until consumed.
  void end();
  Status shutdown(); ///< Tracked shutdown; desired mode becomes SHUTDOWN.
  /// Poll a manual one-shot; never advances an owner job or bypasses its result.
  /// nowMs hook takes precedence over the argument.
  void tick(uint32_t nowMs);
  /// Read configuration fixed bits; this is presence/plausibility, NOT chip ID.
  Status probe();
  /// Reapply/verify all desired settings, including after failed begin or OFFLINE.
  Status recover();
  /// Owner operations: admission, cancellation and result consumption are
  /// bus-silent. A bound transport is required. timeoutMs is 1..INT32_MAX.
  /// IN_PROGRESS means accepted; token remains unchanged on rejection.
  /// Configure preserves the binding/hooks and stages sensor fields privately
  /// until the first write attempt. Once committed, desired settings survive
  /// cancellation/failure and dirty evidence requires a complete recovery.
  Status startInitialize(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token);
  Status startConfigure(const Config& desired, uint32_t nowMs, uint32_t timeoutMs, OperationToken& token);
  Status startRecover(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token);
  Status startShutdown(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token);
  /// Continuous reads the latest value; shutdown starts and completes one-shot.
  /// An existing manual one-shot must be consumed before owner admission.
  Status startRead(uint32_t nowMs, uint32_t timeoutMs, OperationToken& token);
  /// At most maxTransfers physical callbacks; zero budget only checks deadline.
  /// No sleep/yield/retry. nowMs hook is authoritative when present. Otherwise
  /// deadlines use supplied times and settling starts at the first poll AFTER
  /// the triggering write, because callback completion time is not observable.
  /// Callback timeouts are capped to the remaining deadline; without a hook,
  /// their sum within one poll is bounded by that remaining budget.
  /// Terminal results remain until takeResult(); polling never consumes them.
  PollResult poll(uint32_t nowMs, uint8_t maxTransfers = 1);
  Status cancel();
  Status takeResult(OperationToken token, OperationResult& out);
  bool operationActive() const { return _jobActive; }
  bool resultPending() const { return _jobResultPending; }
  OperationSnapshot getOperationSnapshot() const;
  bool isBound() const { return _bound; }
  bool isInitialized() const { return _initialized; }
  DriverState state() const { return _state; }
  DriverState driverState() const { return _state; }
  bool isOnline() const { return _initialized && _state != DriverState::OFFLINE; }
  const Config& getConfig() const { return _config; }
  SettingsSnapshot getSettingsSnapshot() const;
  bool hardwareConfigDirty() const { return _dirty; }
  Status hardwareConfigDirtyError() const { return _dirtyError; }
  /// Counters count individual tracked transport outcomes only. Protocol
  /// mismatches latch dirty state separately, without incrementing bus failures.
  /// Local validation and NOT_READY do not count. Success resets consecutive
  /// failures; lastError retains the last fault. Counters saturate, never wrap.
  uint8_t consecutiveFailures() const { return _consecutiveFailures; }
  uint32_t totalFailures() const { return _totalFailures; }
  uint32_t totalSuccess() const { return _totalSuccess; }
  uint32_t lastOkMs() const { return _lastOkMs; }
  uint32_t lastErrorMs() const { return _lastErrorMs; }
  Status lastError() const { return _lastError; }

  Status setMode(Mode value);
  Status setConversionRate(ConversionRate value);
  Status setExtendedMode(bool value);
  Status setAlertMode(AlertMode value);
  Status setAlertPolarity(AlertPolarity value);
  Status setFaultQueue(FaultQueue value);
  Status setThresholds(float lowC, float highC);
  /// A valid live read is returned even if different from desired config; such
  /// a difference latches dirty state so managed measurement needs recovery.
  /// Observing different EM also requires a fresh-format conversion, even if
  /// hardware later returns to desired EM or a setter adopts the observed EM.
  Status readConfiguration(ConfigurationInfo& out);
  Status readThresholds(float& lowC, float& highC);
  Status verifyConfiguration();
  /// Optional physical GPIO sample. Does not read or acknowledge sensor ALERT.
  Status readAlertPin(bool& active) const;

  /// Latest register value; may be stale/zero after reset or in shutdown.
  /// There is no continuous-mode data-ready flag or sample counter on this IC.
  /// TEMP bit0 selects decoding; mismatch with desired EM returns NOT_READY and
  /// latches dirty/format-refresh evidence. Later managed reads require recovery.
  Status readSample(Sample& out);
  Status readTemperature(float& out);
  /// Requires desired SHUTDOWN mode and clean configuration. No allocation/wait.
  Status startOneShot();
  /// Hardware OS confirmation, gated by conservative conversion time when a
  /// clock is available. Does not infer completion from elapsed time alone.
  Status isConversionReady(bool& ready);
  /// One-shot returns NOT_READY until ready; continuous returns latest sample.
  /// In idle shutdown returns NOT_READY. A successful one-shot read consumes it.
  Status tryRead(Sample& out);
  /// Shutdown: starts or joins a one-shot; continuous: reads latest value.
  /// Requires nowMs. timeoutMs is a polling budget checked between iterations.
  /// A ready iteration can perform two callbacks (CONFIG then TEMP), each bounded
  /// by i2cTimeoutMs, and may finish after the budget. A stalled clock fails after
  /// 1,000,000 unchanged polls.
  Status readBlocking(Sample& out, uint32_t timeoutMs = 100);
  bool conversionStarted() const { return _conversionStarted; }
  bool conversionReady() const { return _conversionReady; }
  bool hasSample() const { return _hasSample; }
  const Sample& lastSample() const { return _lastSample; }
  /// UINT32_MAX if no sample; otherwise unsigned elapsed time. Without a clock
  /// at acquisition the timestamp is zero and age is relative to that epoch.
  uint32_t sampleAgeMs() const;

  Status readRegister(uint8_t reg, uint16_t& out);
  Status writeRegister(uint8_t reg, uint16_t value);
  Status readRegisterRaw(uint8_t reg, uint16_t& out);
  Status writeRegisterRaw(uint8_t reg, uint16_t value);
  static Status decodeTemperature(uint16_t raw, Sample& out);
  static Status encodeThreshold(float celsius, bool extended, uint16_t& out);
  static float decodeThreshold(uint16_t raw, bool extended);
  static ConfigurationInfo decodeConfiguration(uint16_t raw);
  static uint32_t conversionPeriodMs(ConversionRate rate);

private:
  static Status validateConfig(const Config& config);
  static uint16_t encodeConfiguration(const Config& config);
  Status guard(bool clean = false) const;
  Status read(uint8_t reg, uint16_t& out, bool tracked);
  Status write(uint8_t reg, uint16_t value, bool tracked);
  Status track(Status status);
  Status apply();
  enum class ApplyPhase : uint8_t { IDLE, OBSERVE, FIRST_WRITE, WAIT_OLD, SET_FORMAT,
    WRITE_LOW, WRITE_HIGH, TRIGGER, WAIT_NEW, CHECK_NEW, RESTORE, VERIFY_CONFIG, VERIFY_LOW,
    VERIFY_HIGH, COMPLETE };
  struct ApplyState {
    ApplyPhase phase = ApplyPhase::IDLE;
    Config desired{};
    uint16_t previous = 0;
    uint16_t config = 0;
    uint16_t low = 0;
    uint16_t high = 0;
    bool refresh = false;
    bool settle = false;
    bool owner = false;
    bool committed = false;
    uint32_t waitStarted = 0;
    bool waitNeedsAnchor = false;
  };
  void beginApply(const Config& desired, bool owner);
  Status stepApply(bool& transferred);
  bool applyWaiting() const;
  void startApplyWait(ApplyPhase phase);
  Status decodeObservedTemperature(uint16_t raw, Sample& sample);
  enum class JobPhase : uint8_t { IDLE, APPLY, READ_CHECK, READ_TRIGGER, READ_WAIT,
    READ_READY, READ_TEMP, COMPLETE };
  Status admit(OperationKind kind, const Config& desired, uint32_t nowMs,
               uint32_t timeoutMs, OperationToken& token);
  Status finishJob(Status status);
  bool jobLocked() const { return _jobActive || _jobResultPending; }
  bool jobExpired() const;
  uint32_t jobRemainingMs() const;
  uint32_t nextJobPollMs() const;
  bool jobWaiting() const;
  Status stepJob(bool& transferred);
  Status verify(bool tracked);
  Status update(const Config& config);
  void markDirty(Status status);
  void markConfigurationDirty(Status status, uint16_t observed);
  void clearConversion();
  uint32_t now() const;
  bool clockKnown() const { return _config.nowMs != nullptr || _clockSeen; }
  Config _config{};
  bool _bound = false;
  bool _initialized = false;
  DriverState _state = DriverState::UNINIT;
  bool _dirty = false;
  bool _formatRefreshPending = false;
  Status _dirtyError = Status::Ok();
  bool _clockSeen = false;
  uint32_t _tickMs = 0;
  bool _conversionStarted = false;
  bool _conversionReady = false;
  bool _conversionClockKnown = false;
  uint32_t _conversionStartMs = 0;
  bool _hasSample = false;
  Sample _lastSample{};
  uint8_t _consecutiveFailures = 0;
  uint32_t _totalFailures = 0;
  uint32_t _totalSuccess = 0;
  uint32_t _lastOkMs = 0;
  uint32_t _lastErrorMs = 0;
  Status _lastError = Status::Ok();
  ApplyState _apply{};
  OperationToken _nextToken = 0; ///< Deliberately survives bind/unbind.
  OperationToken _jobToken = 0;
  OperationKind _jobKind = OperationKind::NONE;
  JobPhase _jobPhase = JobPhase::IDLE;
  bool _jobActive = false;
  bool _jobResultPending = false;
  bool _jobEffect = false;
  bool _insidePoll = false;
  bool _jobYieldPoll = false;
  bool _jobWaitNeedsAnchor = false;
  uint32_t _jobStartedMs = 0;
  uint32_t _jobTimeoutMs = 0;
  uint32_t _jobWaitStarted = 0;
  uint32_t _callbackTimeoutMs = 0;
  Config _jobDesired{};
  Sample _jobSample{};
  Status _jobStatus = Status::Ok();
  OperationResult _jobResult{};
};
} // namespace TMP1x2
