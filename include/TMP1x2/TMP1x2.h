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
/// Normal calls require successful begin(). OFFLINE is passive health telemetry:
/// calls still access the bus, and successful tracked I/O restores READY.
/// Raw calls and probe() require bind() only and never change health. Raw writes
/// conservatively dirty managed state. Raw CONFIG writes also require a fresh
/// conversion during recovery, so that recovery requires nowMs. No general-call
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
  Status bind(const Config& config);
  void unbind(); ///< Forget all state without touching hardware.
  /// Apply and verify all configuration. An actual EM-format change requires
  /// nowMs; it settles the old conversion then obtains a fresh conversion in
  /// the new format before returning (two conservative 35-ms waits, each with
  /// a 1-ms clock-quantization margin). Entering
  /// shutdown from continuous also requires nowMs to settle the old conversion
  /// (35 ms plus 1-ms margin). Same-format continuous initialization needs no clock callback.
  Status begin(const Config& config);
  /// Local deinitialization only; retains binding/config and never touches I2C.
  void end();
  Status shutdown(); ///< Tracked shutdown; desired mode becomes SHUTDOWN.
  /// Poll an active one-shot. nowMs hook takes precedence over the argument.
  void tick(uint32_t nowMs);
  /// Read configuration fixed bits; this is presence/plausibility, NOT chip ID.
  Status probe();
  /// Reapply/verify all desired settings, including after failed begin or OFFLINE.
  Status recover();
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
  Status readConfiguration(ConfigurationInfo& out);
  Status readThresholds(float& lowC, float& highC);
  Status verifyConfiguration();
  /// Optional physical GPIO sample. Does not read or acknowledge sensor ALERT.
  Status readAlertPin(bool& active) const;

  /// Latest register value; may be stale/zero after reset or in shutdown.
  /// There is no continuous-mode data-ready flag or sample counter on this IC.
  /// TEMP bit0 selects decoding; mismatch with desired EM returns NOT_READY.
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
  Status waitConversionInterval();
  Status verify(bool tracked);
  Status update(const Config& config);
  void markDirty(Status status);
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
};
} // namespace TMP1x2
