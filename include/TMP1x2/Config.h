/// @file Config.h
/// @brief Application-owned transport and typed sensor configuration.
#pragma once
#include <cstddef>
#include <cstdint>
#include "TMP1x2/Status.h"
namespace TMP1x2 {
/// Callbacks must honor timeoutMs, serialize bus access, and never re-enter this
/// instance. Return a terminal result; IN_PROGRESS is normalized to I2C_ERROR.
/// The core neither initializes nor destroys the bus or GPIOs.
using I2cWriteFn = Status (*)(uint8_t, const uint8_t*, size_t, uint32_t, void*);
/// Must issue an atomic pointer write + repeated START + read, MSB first.
using I2cWriteReadFn = Status (*)(uint8_t, const uint8_t*, size_t, uint8_t*, size_t,
                                  uint32_t, void*);
using NowMsFn = uint32_t (*)(void*);
using YieldFn = void (*)(void*);
using GpioReadFn = bool (*)(int, void*);
/// Explicit BOM choice; neither part contains a readable device identifier.
/// Supports classic 0x48..0x4B parts. TMP112D address-select 0x40..0x43 variants
/// are outside this API's scope and rejected by address validation.
enum class Model : uint8_t { TMP102 = 0, TMP112 = 1 };
enum class Mode : uint8_t { CONTINUOUS = 0, SHUTDOWN = 1 };
enum class ConversionRate : uint8_t { HZ_0_25 = 0, HZ_1 = 1, HZ_4 = 2, HZ_8 = 3 };
enum class AlertMode : uint8_t { COMPARATOR = 0, INTERRUPT_MODE = 1 };
enum class AlertPolarity : uint8_t { ACTIVE_LOW = 0, ACTIVE_HIGH = 1 };
enum class FaultQueue : uint8_t { FAULTS_1 = 0, FAULTS_2 = 1, FAULTS_4 = 2, FAULTS_6 = 3 };
constexpr const char* toString(Model v) {
  return v == Model::TMP102 ? "TMP102" : v == Model::TMP112 ? "TMP112" : "UNKNOWN";
}
constexpr const char* toString(Mode v) {
  return v == Mode::CONTINUOUS ? "CONTINUOUS" : v == Mode::SHUTDOWN ? "SHUTDOWN" : "UNKNOWN";
}
constexpr const char* toString(ConversionRate v) {
  return v == ConversionRate::HZ_0_25 ? "0.25 Hz" : v == ConversionRate::HZ_1 ? "1 Hz" :
         v == ConversionRate::HZ_4 ? "4 Hz" : v == ConversionRate::HZ_8 ? "8 Hz" : "UNKNOWN";
}
constexpr const char* toString(AlertMode v) {
  return v == AlertMode::COMPARATOR ? "COMPARATOR" : v == AlertMode::INTERRUPT_MODE ? "INTERRUPT" : "UNKNOWN";
}
constexpr const char* toString(AlertPolarity v) {
  return v == AlertPolarity::ACTIVE_LOW ? "ACTIVE_LOW" : v == AlertPolarity::ACTIVE_HIGH ? "ACTIVE_HIGH" : "UNKNOWN";
}
constexpr const char* toString(FaultQueue v) {
  return v == FaultQueue::FAULTS_1 ? "1" : v == FaultQueue::FAULTS_2 ? "2" :
         v == FaultQueue::FAULTS_4 ? "4" : v == FaultQueue::FAULTS_6 ? "6" : "UNKNOWN";
}
struct Config {
  I2cWriteFn i2cWrite = nullptr;
  I2cWriteReadFn i2cWriteRead = nullptr;
  void* i2cUser = nullptr;
  /// Optional authoritative unsigned monotonic clock; wraparound supported.
  /// Without it tick(nowMs) supplies the clock and blocking reads are disabled.
  /// An actual EM change or continuous-to-shutdown transition requires nowMs
  /// so synchronous configuration can settle in-flight conversions safely.
  NowMsFn nowMs = nullptr;
  YieldFn cooperativeYield = nullptr;
  void* timeUser = nullptr;
  Model model = Model::TMP102;
  uint8_t i2cAddress = 0x48;
  uint32_t i2cTimeoutMs = 50;
  Mode mode = Mode::CONTINUOUS;
  ConversionRate conversionRate = ConversionRate::HZ_4;
  bool extendedMode = false;
  AlertMode alertMode = AlertMode::COMPARATOR;
  AlertPolarity alertPolarity = AlertPolarity::ACTIVE_LOW;
  FaultQueue faultQueue = FaultQueue::FAULTS_1;
  /// Rounded to nearest 0.0625 C (ties away from zero). Register representable
  /// range: -128..127.9375 C normal, -256..255.9375 C extended. These register
  /// ranges do not expand the device's specified measurement accuracy/range.
  float lowThresholdC = 75.0f;
  float highThresholdC = 80.0f;
  int alertPin = -1;
  GpioReadFn gpioRead = nullptr;
  void* gpioUser = nullptr;
  /// Zero normalized to one by bind()/begin().
  uint8_t offlineThreshold = 5;
};
} // namespace TMP1x2
