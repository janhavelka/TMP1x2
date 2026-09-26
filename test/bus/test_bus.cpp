#include "TMP1x2/BusOperations.h"
#include "TMP1x2/CommandTable.h"
#include <cstdio>

namespace t = TMP1x2;
namespace ops = TMP1x2::BusOperations;

#define CHECK(condition) do { if (!(condition)) { \
  std::printf("[FAIL] bus:%d %s\n", __LINE__, #condition); return false; } } while (false)

struct Bus {
  unsigned calls = 0;
  uint8_t address = 0xFF;
  uint8_t command = 0xFF;
  size_t length = 0;
  uint32_t timeout = 0;
  uint8_t response = 0x90;
  bool fillResponse = true;
  t::Status result = t::Status::Ok();

  static t::Status write(uint8_t address, const uint8_t* tx, size_t size,
                         uint32_t timeout, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    ++bus.calls; bus.address = address; bus.length = size; bus.timeout = timeout;
    bus.command = size == 1 && tx != nullptr ? tx[0] : 0xFF;
    return bus.result;
  }

  static t::Status receive(uint8_t address, uint8_t* rx, size_t size,
                           uint32_t timeout, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    ++bus.calls; bus.address = address; bus.length = size; bus.timeout = timeout;
    if (size == 1 && rx != nullptr && bus.fillResponse) rx[0] = bus.response;
    return bus.result;
  }
};

static bool same(const ops::AlertResponse& a, const ops::AlertResponse& b) {
  return a.raw == b.raw && a.address == b.address && a.alertStatusBit == b.alertStatusBit;
}

static bool framingAndAdmission() {
  Bus bus;
  ops::AlertResponse out{0x97, 0x4B, true};
  const auto sentinel = out;
  CHECK(ops::generalCallReset(nullptr, &bus, 1).is(t::Err::INVALID_PARAM));
  CHECK(ops::readAlertResponse(nullptr, &bus, 1, out).is(t::Err::INVALID_PARAM));
  const uint32_t invalid[] = {0, 0x80000000u, 0xFFFFFFFFu};
  for (const auto timeout : invalid) {
    CHECK(ops::generalCallReset(Bus::write, &bus, timeout).is(t::Err::INVALID_PARAM));
    CHECK(ops::readAlertResponse(Bus::receive, &bus, timeout, out).is(t::Err::INVALID_PARAM));
    CHECK(same(out, sentinel));
  }
  CHECK(bus.calls == 0);
  CHECK(ops::generalCallReset(Bus::write, &bus, 23).ok());
  CHECK(bus.calls == 1); CHECK(bus.address == 0); CHECK(bus.command == 6);
  CHECK(bus.length == 1); CHECK(bus.timeout == 23);
  CHECK(ops::readAlertResponse(Bus::receive, &bus, 0x7FFFFFFFu, out).ok());
  CHECK(bus.calls == 2); CHECK(bus.address == 0x0C); CHECK(bus.length == 1);
  CHECK(bus.timeout == 0x7FFFFFFFu); CHECK(out.raw == 0x90); CHECK(out.address == 0x48);
  CHECK(!out.alertStatusBit);
  bus.response = 0x87; // TMP112D ADD0 package, status=1, no physical ALERT pin.
  CHECK(ops::readAlertResponse(Bus::receive, &bus, 1, out).ok());
  CHECK(bus.calls == 3); CHECK(out.address == 0x43); CHECK(out.alertStatusBit);
  return true;
}

static bool transportFailureAndOutput() {
  // A failing receive is allowed to have changed its scratch buffer, and a
  // failing reset may already have reached hardware. Neither operation retries.
  const t::Err errors[] = {t::Err::I2C_ERROR, t::Err::I2C_NACK_ADDR, t::Err::I2C_NACK_DATA,
      t::Err::I2C_TIMEOUT, t::Err::I2C_BUS, t::Err::TIMEOUT, t::Err::BUSY,
      t::Err::CANCELLED, t::Err::IN_PROGRESS};
  for (const auto error : errors) {
    Bus bus;
    bus.result = t::Status::Error(error, "transport evidence", 731);
    ops::AlertResponse out{0x93, 0x49, true}; const auto before = out;
    auto status = ops::generalCallReset(Bus::write, &bus, 11);
    CHECK(bus.calls == 1);
    CHECK(status.code == (error == t::Err::IN_PROGRESS ? t::Err::I2C_ERROR : error));
    CHECK(status.detail == 731);
    if (error != t::Err::IN_PROGRESS) CHECK(status.msg == bus.result.msg);
    status = ops::readAlertResponse(Bus::receive, &bus, 11, out);
    CHECK(bus.calls == 2); CHECK(same(out, before));
    CHECK(status.code == (error == t::Err::IN_PROGRESS ? t::Err::I2C_ERROR : error));
    CHECK(status.detail == 731);
    if (error != t::Err::IN_PROGRESS) CHECK(status.msg == bus.result.msg);
  }
  Bus bus; bus.fillResponse = false;
  ops::AlertResponse out{0x93, 0x49, true}; const auto before = out;
  CHECK(ops::readAlertResponse(Bus::receive, &bus, 1, out).is(t::Err::CONFIG_MISMATCH));
  CHECK(bus.calls == 1); CHECK(same(out, before));
  return true;
}

static bool exhaustiveResponseBytes() {
  Bus bus;
  for (unsigned value = 0; value < 256; ++value) {
    bus.response = static_cast<uint8_t>(value);
    const unsigned address = value / 2;
    const bool valid = address >= 0x08 && address <= 0x77 && address != 0x0C;
    ops::AlertResponse decoded{0x95, 0x4A, true}; const auto before = decoded;
    const auto decodeStatus = ops::decodeAlertResponse(bus.response, decoded);
    CHECK(decodeStatus.ok() == valid);
    if (valid) {
      CHECK(decoded.raw == value); CHECK(decoded.address == address);
      CHECK(decoded.alertStatusBit == ((value % 2) != 0));
    } else {
      CHECK(decodeStatus.is(t::Err::CONFIG_MISMATCH)); CHECK(decodeStatus.detail == static_cast<int32_t>(value));
      CHECK(same(decoded, before));
    }
    ops::AlertResponse read = before;
    const auto readStatus = ops::readAlertResponse(Bus::receive, &bus, 7, read);
    CHECK(bus.calls == value + 1); CHECK(readStatus.code == decodeStatus.code);
    CHECK(same(read, decoded));
  }
  // A non-TMP responder is valid ARA data, with no claimed model identity.
  ops::AlertResponse other; CHECK(ops::decodeAlertResponse(0x61, other).ok());
  CHECK(other.address == 0x30); CHECK(other.alertStatusBit);
  ops::AlertCause cause = ops::AlertCause::LOW_THRESHOLD;
  CHECK(ops::decodeAlertCause(other, t::Model::TMP102, t::AlertPolarity::ACTIVE_LOW,
                              cause).is(t::Err::INVALID_PARAM));
  CHECK(cause == ops::AlertCause::LOW_THRESHOLD);
  return true;
}

static bool modelSpecificCause() {
  const t::Model models[] = {t::Model::TMP102, t::Model::TMP112, t::Model::TMP112D_ADDRESS_SELECT};
  for (const auto model : models) {
    const unsigned first = model == t::Model::TMP112D_ADDRESS_SELECT ? 0x40 : 0x48;
    for (unsigned address = first; address < first + 4; ++address) {
      for (unsigned polarity = 0; polarity < 2; ++polarity) {
        for (unsigned bit = 0; bit < 2; ++bit) {
          ops::AlertResponse response;
          CHECK(ops::decodeAlertResponse(static_cast<uint8_t>(address * 2 + bit), response).ok());
          ops::AlertCause cause = static_cast<ops::AlertCause>(255);
          CHECK(ops::decodeAlertCause(response, model, static_cast<t::AlertPolarity>(polarity), cause).ok());
          // Deliberately independent truth tables from each published datasheet.
          const bool highTmp102[2][2] = {{true, false}, {false, true}};
          const bool expectedHigh = model == t::Model::TMP102 ? highTmp102[polarity][bit] : bit == 1;
          CHECK(cause == (expectedHigh ? ops::AlertCause::HIGH_THRESHOLD : ops::AlertCause::LOW_THRESHOLD));
        }
      }
    }
  }
  ops::AlertResponse response; CHECK(ops::decodeAlertResponse(0x90, response).ok());
  ops::AlertCause cause = ops::AlertCause::LOW_THRESHOLD;
  CHECK(ops::decodeAlertCause(response, static_cast<t::Model>(255), t::AlertPolarity::ACTIVE_LOW,
                              cause).is(t::Err::INVALID_PARAM));
  CHECK(cause == ops::AlertCause::LOW_THRESHOLD);
  CHECK(ops::decodeAlertCause(response, t::Model::TMP102, static_cast<t::AlertPolarity>(2),
                              cause).is(t::Err::INVALID_PARAM));
  CHECK(cause == ops::AlertCause::LOW_THRESHOLD);
  response.raw = 0x92; // Forged/inconsistent view must not be interpreted.
  CHECK(ops::decodeAlertCause(response, t::Model::TMP102, t::AlertPolarity::ACTIVE_LOW,
                              cause).is(t::Err::INVALID_PARAM));
  CHECK(cause == ops::AlertCause::LOW_THRESHOLD);
  response.raw = 0x91; // Address matches, status bit does not.
  CHECK(ops::decodeAlertCause(response, t::Model::TMP102, t::AlertPolarity::ACTIVE_LOW,
                              cause).is(t::Err::INVALID_PARAM));
  CHECK(cause == ops::AlertCause::LOW_THRESHOLD);
  return true;
}

int main() {
  static_assert(t::cmd::GENERAL_CALL_ADDRESS == 0x00, "7-bit general call");
  static_assert(t::cmd::GENERAL_CALL_RESET == 0x06, "reset data byte");
  static_assert(t::cmd::SMBUS_ALERT_RESPONSE_ADDRESS == 0x0C, "7-bit ARA, wire read byte 0x19");
  if (!framingAndAdmission() || !transportFailureAndOutput() || !exhaustiveResponseBytes() ||
      !modelSpecificCause()) return 1;
  std::puts("[PASS] shared-bus framing, failures, all response bytes and model-specific alert causes");
  return 0;
}
