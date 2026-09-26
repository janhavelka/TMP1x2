#include "TMP1x2/BusOperations.h"
#include "TMP1x2/CommandTable.h"

namespace TMP1x2 { namespace BusOperations {
namespace {
bool validTimeout(uint32_t timeoutMs) {
  return timeoutMs != 0 && timeoutMs <= 0x7FFFFFFFu;
}

Status terminalStatus(Status status) {
  if (status.inProgress())
    return Status::Error(Err::I2C_ERROR, "Blocking transport returned IN_PROGRESS", status.detail);
  return status;
}
} // namespace

Status generalCallReset(I2cWriteFn write, void* user, uint32_t timeoutMs) {
  if (write == nullptr || !validTimeout(timeoutMs))
    return Status::Error(Err::INVALID_PARAM, "Reset requires write callback and bounded timeout");
  const uint8_t command = cmd::GENERAL_CALL_RESET;
  return terminalStatus(write(cmd::GENERAL_CALL_ADDRESS, &command, 1, timeoutMs, user));
}

Status decodeAlertResponse(uint8_t raw, AlertResponse& out) {
  const auto address = static_cast<uint8_t>(raw >> 1U);
  if (address < 0x08 || address > 0x77 || address == cmd::SMBUS_ALERT_RESPONSE_ADDRESS)
    return Status::Error(Err::CONFIG_MISMATCH, "Invalid SMBus alert response address", raw);
  out = AlertResponse{raw, address, (raw & 1U) != 0};
  return Status::Ok();
}

Status readAlertResponse(ReceiveFn receive, void* user, uint32_t timeoutMs,
                         AlertResponse& out) {
  if (receive == nullptr || !validTimeout(timeoutMs))
    return Status::Error(Err::INVALID_PARAM, "ARA requires receive callback and bounded timeout");
  uint8_t raw = 0; // An OK callback that failed to fill its buffer is rejected.
  const auto status = terminalStatus(receive(cmd::SMBUS_ALERT_RESPONSE_ADDRESS, &raw, 1,
                                             timeoutMs, user));
  if (!status.ok()) return status;
  return decodeAlertResponse(raw, out);
}

Status decodeAlertCause(const AlertResponse& response, Model model,
                        AlertPolarity polarity, AlertCause& out) {
  if (!isValidAddress(model, response.address) ||
      (polarity != AlertPolarity::ACTIVE_LOW && polarity != AlertPolarity::ACTIVE_HIGH) ||
      response.address != static_cast<uint8_t>(response.raw >> 1U) ||
      response.alertStatusBit != ((response.raw & 1U) != 0))
    return Status::Error(Err::INVALID_PARAM, "ARA cause requires consistent response and known model/polarity");
  const bool high = model == Model::TMP102 ?
      response.alertStatusBit == (polarity == AlertPolarity::ACTIVE_HIGH) : response.alertStatusBit;
  out = high ? AlertCause::HIGH_THRESHOLD : AlertCause::LOW_THRESHOLD;
  return Status::Ok();
}

} } // namespace TMP1x2::BusOperations
