/// @file BusOperations.h
/// @brief Explicit shared-bus commands, separate from per-device driver state.
#pragma once
#include "TMP1x2/Config.h"

namespace TMP1x2 { namespace BusOperations {

/// One receive-only transaction: START, address+read, rxLen bytes, final NACK,
/// STOP. There is no register-pointer write. Return OK only after receiving all
/// requested bytes. The callback must complete synchronously within timeoutMs,
/// serialize bus access, and return a terminal Status; IN_PROGRESS is rejected.
using ReceiveFn = Status (*)(uint8_t address, uint8_t* rx, size_t rxLen,
                            uint32_t timeoutMs, void* user);

struct AlertResponse {
  uint8_t raw = 0;          ///< Complete response, including the status LSB.
  uint8_t address = 0;      ///< Responding 7-bit address; does not identify a model.
  bool alertStatusBit = false; ///< Device-specific status, not a fixed R/W bit.
};

enum class AlertCause : uint8_t { HIGH_THRESHOLD = 0, LOW_THRESHOLD = 1 };
constexpr const char* toString(AlertCause value) {
  return value == AlertCause::HIGH_THRESHOLD ? "HIGH_THRESHOLD" :
         value == AlertCause::LOW_THRESHOLD ? "LOW_THRESHOLD" : "UNKNOWN";
}

/// Write exactly {0x06} to the general-call address 0x00. This resets EVERY
/// compatible device on the bus; success does not identify which devices reset.
/// The application must authorize this bus-wide effect and stop all device work
/// before calling. After any attempted transfer, including an ambiguous error,
/// invalidateDeviceState() and reinitialize ALL affected driver instances before
/// use. Cancel/consume outstanding owner results first; end() alone is not an
/// external-reset notification and cannot establish a fresh post-reset sample.
/// This helper owns no instances and cannot invalidate their caches itself.
/// Performs one bounded callback, with no retry, delay, or implicit recovery.
/// Missing callbacks or timeout outside 1..INT32_MAX fail without bus access.
Status generalCallReset(I2cWriteFn write, void* user, uint32_t timeoutMs);

/// Read one response byte from SMBus Alert Response Address 0x0C. In interrupt
/// mode the lowest-address alerting device wins arbitration; this acknowledges
/// its alert. Other responders remain pending. One call obtains one winner,
/// never drains the bus. A failed/invalid response can still have cleared a
/// hardware alert. TMP112D address-select variants support ARA without an ALERT
/// pin. There is no device-health accounting or cache update here.
/// Accepts other devices' unicast addresses without assigning TMP semantics.
/// Output is unchanged on failure. Same timeout/terminal-callback requirements
/// as generalCallReset; the application owns bus serialization and GPIOs.
Status readAlertResponse(ReceiveFn receive, void* user, uint32_t timeoutMs,
                         AlertResponse& out);

/// Pure decoder: preserve both status-bit values, reject reserved I2C addresses
/// (0x00..0x07, 0x78..0x7F) and the ARA address itself. Invalid response returns
/// CONFIG_MISMATCH with the raw byte in detail; out remains unchanged.
Status decodeAlertResponse(uint8_t raw, AlertResponse& out);

/// Interpret a response only when the caller knows the responding device's
/// model and configured polarity. Address/model matching is necessary but does
/// not establish device identity. Invalid model/polarity, inconsistent fields,
/// or an address outside that model's range leaves out unchanged.
/// TMP102: status==POL means high threshold (TI SBOS397I section 6.3.7).
/// TMP112 family: status=1 means high threshold (TI SBOS473L section 7.3.2.5,
/// which specifies no polarity inversion). The raw response remains available
/// because these published model semantics differ. Neither mapping describes
/// the current comparator AL bit or physical ALERT level after acknowledgement.
Status decodeAlertCause(const AlertResponse& response, Model model,
                        AlertPolarity polarity, AlertCause& out);

} } // namespace TMP1x2::BusOperations
