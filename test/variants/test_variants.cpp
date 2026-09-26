#include "TMP1x2/TMP1x2.h"
#include <cstdio>

namespace t = TMP1x2;
#define CHECK(condition) do { if (!(condition)) { \
  std::printf("[FAIL] variants:%d %s\n", __LINE__, #condition); return 1; } } while (false)

struct Bus {
  uint16_t registers[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  unsigned calls = 0;
  unsigned gpioCalls = 0;
  uint8_t lastAddress = 0;
  bool gpioLevel = false;
  static t::Status write(uint8_t address, const uint8_t* tx, size_t size, uint32_t, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    ++bus.calls; bus.lastAddress = address;
    if (size != 3 || tx[0] < 1 || tx[0] > 3)
      return t::Status::Error(t::Err::I2C_ERROR, "invalid write frame");
    const auto raw = static_cast<uint16_t>((static_cast<uint16_t>(tx[1]) << 8U) | tx[2]);
    bus.registers[tx[0]] = tx[0] == 1 ? static_cast<uint16_t>((raw & 0x1FD0U) | 0x6020U) : raw;
    return t::Status::Ok();
  }
  static t::Status read(uint8_t address, const uint8_t* tx, size_t size,
                        uint8_t* rx, size_t count, uint32_t, void* user) {
    auto& bus = *static_cast<Bus*>(user);
    ++bus.calls; bus.lastAddress = address;
    if (size != 1 || count != 2 || tx[0] > 3)
      return t::Status::Error(t::Err::I2C_ERROR, "invalid read frame");
    const auto raw = bus.registers[tx[0]];
    rx[0] = static_cast<uint8_t>(raw >> 8U); rx[1] = static_cast<uint8_t>(raw);
    return t::Status::Ok();
  }
  static bool gpio(int, void* user) {
    auto& bus = *static_cast<Bus*>(user); ++bus.gpioCalls; return bus.gpioLevel;
  }
  t::Config config(t::Model model, uint8_t address) {
    t::Config config;
    config.model = model; config.i2cAddress = address;
    config.i2cWrite = write; config.i2cWriteRead = read; config.i2cUser = this;
    config.gpioRead = gpio; config.gpioUser = this;
    return config;
  }
};

int main() {
  static_assert(static_cast<unsigned>(t::Model::TMP102) == 0, "existing model values stay stable");
  static_assert(static_cast<unsigned>(t::Model::TMP112) == 1, "existing model values stay stable");
  static_assert(t::modelAddressMin(t::Model::TMP112D_ADDRESS_SELECT) == 0x40, "TI address-select range");
  static_assert(t::modelAddressMax(t::Model::TMP112D_ADDRESS_SELECT) == 0x43, "TI address-select range");
  static_assert(!t::hasAlertOutput(t::Model::TMP112D_ADDRESS_SELECT), "ADD0 replaces ALERT on X2SON");
  const t::Model models[] = {t::Model::TMP102, t::Model::TMP112, t::Model::TMP112D_ADDRESS_SELECT};
  for (const auto model : models) {
    // Exhaust addresses so reserved gaps and the other package's range cannot
    // accidentally be accepted by a broad 0x40..0x4B range check.
    for (unsigned address = 0; address < 128; ++address) {
      Bus bus; t::TMP1x2 device;
      const auto addr = static_cast<uint8_t>(address);
      const bool expected = model == t::Model::TMP112D_ADDRESS_SELECT ?
          address >= 0x40 && address <= 0x43 : address >= 0x48 && address <= 0x4B;
      CHECK(t::isValidAddress(model, addr) == expected);
      const auto status = device.bind(bus.config(model, addr));
      CHECK(status.ok() == expected); CHECK(bus.calls == 0);
      if (expected) {
        CHECK(device.probe().ok()); CHECK(bus.lastAddress == address);
        CHECK(device.totalSuccess() == 0); // Diagnostics remain outside health.
        CHECK(device.begin(bus.config(model, addr)).ok());
        t::Sample sample; CHECK(device.readSample(sample).ok()); CHECK(sample.celsius == 25.0f);
        CHECK(bus.lastAddress == address);
      }
    }
  }
  Bus bus; t::TMP1x2 device;
  auto config = bus.config(t::Model::TMP112D_ADDRESS_SELECT, 0x40);
  config.alertPin = 6;
  CHECK(device.bind(config).is(t::Err::INVALID_CONFIG)); CHECK(bus.calls == 0);
  config.alertPin = -1;
  CHECK(device.begin(config).ok());
  const auto calls = bus.calls;
  bool active = true;
  CHECK(!device.readAlertPin(active).ok()); CHECK(active); CHECK(bus.calls == calls); CHECK(bus.gpioCalls == 0);
  device.unbind();
  config = bus.config(t::Model::TMP112, 0x48); config.alertPin = 6;
  CHECK(device.begin(config).ok()); CHECK(device.readAlertPin(active).ok()); CHECK(active);
  bus.gpioLevel = true;
  CHECK(device.readAlertPin(active).ok()); CHECK(!active);
  config.model = static_cast<t::Model>(255);
  CHECK(!t::isValidModel(config.model)); CHECK(!t::isValidAddress(config.model, 0));
  CHECK(!t::hasAlertOutput(config.model));
  CHECK(device.bind(config).is(t::Err::INVALID_CONFIG));
  std::puts("[PASS] model address and ALERT capabilities");
  return 0;
}
