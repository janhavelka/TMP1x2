/// @file Status.h
/// @brief Portable status codes; messages have static lifetime.
#pragma once
#include <cstdint>

namespace TMP1x2 {
enum class Err : uint8_t {
  OK = 0, NOT_INITIALIZED, INVALID_CONFIG, I2C_ERROR, TIMEOUT,
  INVALID_PARAM, DEVICE_NOT_FOUND, CONFIG_MISMATCH,
  MEASUREMENT_NOT_READY, CONVERSION_NOT_READY = MEASUREMENT_NOT_READY,
  BUSY, IN_PROGRESS, I2C_NACK_ADDR, I2C_NACK_DATA, I2C_TIMEOUT, I2C_BUS,
  OFFLINE, NOT_BOUND,
  CANCELLED, OPERATION_TIMEOUT, RESULT_NOT_AVAILABLE, TOKEN_MISMATCH
};
constexpr const char* errorName(Err err) {
  switch (err) {
    case Err::OK: return "OK";
    case Err::NOT_INITIALIZED: return "NOT_INITIALIZED";
    case Err::INVALID_CONFIG: return "INVALID_CONFIG";
    case Err::I2C_ERROR: return "I2C_ERROR";
    case Err::TIMEOUT: return "TIMEOUT";
    case Err::INVALID_PARAM: return "INVALID_PARAM";
    case Err::DEVICE_NOT_FOUND: return "DEVICE_NOT_FOUND";
    case Err::CONFIG_MISMATCH: return "CONFIG_MISMATCH";
    case Err::MEASUREMENT_NOT_READY: return "MEASUREMENT_NOT_READY";
    case Err::BUSY: return "BUSY";
    case Err::IN_PROGRESS: return "IN_PROGRESS";
    case Err::I2C_NACK_ADDR: return "I2C_NACK_ADDR";
    case Err::I2C_NACK_DATA: return "I2C_NACK_DATA";
    case Err::I2C_TIMEOUT: return "I2C_TIMEOUT";
    case Err::I2C_BUS: return "I2C_BUS";
    case Err::OFFLINE: return "OFFLINE";
    case Err::NOT_BOUND: return "NOT_BOUND";
    case Err::CANCELLED: return "CANCELLED";
    case Err::OPERATION_TIMEOUT: return "OPERATION_TIMEOUT";
    case Err::RESULT_NOT_AVAILABLE: return "RESULT_NOT_AVAILABLE";
    case Err::TOKEN_MISMATCH: return "TOKEN_MISMATCH";
  }
  return "UNKNOWN";
}
constexpr const char* toString(Err err) { return errorName(err); }
struct Status {
  Err code = Err::OK;
  int32_t detail = 0;
  const char* msg = "";
  constexpr Status() = default;
  constexpr Status(Err value, int32_t detailValue, const char* message)
      : code(value), detail(detailValue), msg(message) {}
  constexpr bool ok() const { return code == Err::OK; }
  constexpr bool is(Err value) const { return code == value; }
  constexpr bool inProgress() const { return is(Err::IN_PROGRESS); }
  explicit constexpr operator bool() const { return ok(); }
  static constexpr Status Ok() { return Status{Err::OK, 0, "OK"}; }
  static constexpr Status Error(Err value, const char* message, int32_t detail = 0) {
    return Status{value, detail, message};
  }
};
} // namespace TMP1x2
