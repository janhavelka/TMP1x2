#include "TMP1x2/TMP1x2.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace t = TMP1x2;
static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("[FAIL] %s:%d %s\n", __func__, __LINE__, #expr); ++failures; return; } } while (false)

struct Device {
  uint16_t registers[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  uint32_t ms = 0;
  unsigned calls = 0;
  unsigned failAt = 0;
  unsigned changeAt = 0;
  uint16_t changeMask = 0x40;
  bool shot = false;
  uint32_t shotStarted = 0;
  static uint32_t clock(void* user) { return static_cast<Device*>(user)->ms; }
  static void yield(void* user) { ++static_cast<Device*>(user)->ms; }
  void store(uint16_t value) {
    const bool extended = (value & 0x10U) != 0;
    registers[0] = static_cast<uint16_t>((registers[0] & 0xFFFEU) | (extended ? 1U : 0U));
    registers[1] = static_cast<uint16_t>((value & 0x1FD0U) | 0x6020U);
    shot = (value & 0x8100U) == 0x8100U;
    if (shot) shotStarted = ms;
    else if ((value & 0x100U) != 0) registers[1] |= 0x8000U;
  }
  static t::Status write(uint8_t, const uint8_t* data, size_t size, uint32_t timeout, void* user) {
    auto& d = *static_cast<Device*>(user); ++d.calls;
    if (size != 3 || data[0] < 1 || data[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "framing");
    if (d.calls == d.failAt) return t::Status::Error(t::Err::I2C_TIMEOUT, "write fault", 91);
    const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8U) | data[2]);
    if (data[0] == 1) d.store(value); else d.registers[data[0]] = value;
    return t::Status::Ok();
  }
  static t::Status read(uint8_t, const uint8_t* data, size_t size, uint8_t* out, size_t count,
                        uint32_t timeout, void* user) {
    auto& d = *static_cast<Device*>(user); ++d.calls;
    if (size != 1 || count != 2 || data[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "framing");
    if (d.calls == d.failAt) { out[0] = out[1] = 0xFF; return t::Status::Error(t::Err::I2C_TIMEOUT, "read fault", 91); }
    if (d.shot && static_cast<uint32_t>(d.ms - d.shotStarted) >= 35U) {
      d.shot = false; d.registers[1] |= 0x8000U;
      d.registers[0] = (d.registers[1] & 0x10U) != 0 ? 0x0C81 : 0x1900;
    }
    if (d.calls == d.changeAt) d.registers[1] ^= d.changeMask;
    const uint16_t value = d.registers[data[0]];
    out[0] = static_cast<uint8_t>(value >> 8U); out[1] = static_cast<uint8_t>(value);
    return t::Status::Ok();
  }
  t::Config config(bool clocked = true) {
    t::Config c;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cUser = this;
    if (clocked) { c.nowMs = clock; c.cooperativeYield = yield; }
    c.timeUser = this;
    return c;
  }
};

static bool complete(t::TMP1x2& driver, Device& device, t::OperationToken token, t::OperationResult& result) {
  for (unsigned count = 0; count < 2000 && driver.operationActive(); ++count) {
    driver.poll(device.ms, 1); ++device.ms;
  }
  return driver.takeResult(token, result).ok() && result.status.ok();
}

static void validationAndEncoding() {
  Device d; auto c = d.config();
  CHECK(t::TMP1x2::validateConfig(c).ok()); CHECK(d.calls == 0);
  c.i2cWrite = nullptr;
  CHECK(t::TMP1x2::validateConfig(c).is(t::Err::INVALID_CONFIG));
  CHECK(t::TMP1x2::validateSettings(c).ok());
  c.mode = t::Mode::SHUTDOWN; c.conversionRate = t::ConversionRate::HZ_8;
  c.alertMode = t::AlertMode::INTERRUPT_MODE; c.alertPolarity = t::AlertPolarity::ACTIVE_HIGH;
  c.faultQueue = t::FaultQueue::FAULTS_6; c.extendedMode = true;
  c.lowThresholdC = 1.03125f;
  uint16_t value = 0, mask = 0;
  CHECK(t::TMP1x2::encodeConfiguration(c, value).ok()); CHECK(value == 0x7FD0U);
  CHECK((value & t::cmd::MASK_OS) == 0);
  CHECK(t::TMP1x2::expectedConfigurationRegister(c, 1, value, mask).ok()); CHECK(value == 0x7FD0U && mask == 0x1FD0U);
  CHECK(t::TMP1x2::expectedConfigurationRegister(c, 2, value, mask).ok()); CHECK(value == 0x0088U && mask == 0xFFFFU);
  CHECK(t::TMP1x2::expectedConfigurationRegister(c, 3, value, mask).ok()); CHECK(value == 0x2800U && mask == 0xFFFFU);
  value = 0xA55A; mask = 0x1234;
  CHECK(t::TMP1x2::expectedConfigurationRegister(c, 0, value, mask).is(t::Err::INVALID_PARAM));
  CHECK(value == 0xA55A && mask == 0x1234);
  c.faultQueue = static_cast<t::FaultQueue>(99);
  CHECK(!t::TMP1x2::encodeConfiguration(c, value).ok()); CHECK(value == 0xA55A);
  c.faultQueue = t::FaultQueue::FAULTS_1; c.lowThresholdC = std::numeric_limits<float>::quiet_NaN();
  CHECK(!t::TMP1x2::expectedConfigurationRegister(c, 1, value, mask).ok());
  CHECK(value == 0xA55A && mask == 0x1234); CHECK(d.calls == 0);
  const auto normal = t::TMP1x2::defaultConfig();
  const auto selected = t::TMP1x2::defaultConfig(t::Model::TMP112D_ADDRESS_SELECT);
  CHECK(normal.model == t::Model::TMP102 && normal.i2cAddress == 0x48);
  CHECK(selected.model == t::Model::TMP112D_ADDRESS_SELECT && selected.i2cAddress == 0x40 && selected.alertPin == -1);
  CHECK(!t::TMP1x2::validateConfig(selected).ok()); // Transport still belongs to caller.
}

static void unitConversions() {
  CHECK(t::TMP1x2::celsiusToFahrenheit(-40) == -40);
  CHECK(t::TMP1x2::celsiusToFahrenheit(0) == 32);
  CHECK(t::TMP1x2::celsiusToFahrenheit(100) == 212);
  CHECK(std::fabs(t::TMP1x2::fahrenheitToCelsius(77) - 25) < 0.00001f);
  CHECK(t::TMP1x2::temperatureResolutionC() == 0.0625f);
  CHECK(t::TMP1x2::minimumThresholdC(false) == -128 && t::TMP1x2::maximumThresholdC(false) == 127.9375f);
  CHECK(t::TMP1x2::minimumThresholdC(true) == -256 && t::TMP1x2::maximumThresholdC(true) == 255.9375f);
  CHECK(t::TMP1x2::conversionTimeMaxMs() == 35);
  CHECK(t::TMP1x2::conversionRateHz(t::ConversionRate::HZ_0_25) == 0.25f);
  CHECK(t::TMP1x2::conversionRateHz(static_cast<t::ConversionRate>(99)) == 0);
  CHECK(t::TMP1x2::faultQueueCount(t::FaultQueue::FAULTS_6) == 6);
  CHECK(t::TMP1x2::faultQueueCount(static_cast<t::FaultQueue>(99)) == 0);
  for (unsigned extended = 0; extended < 2; ++extended) {
    const int maximum = extended != 0 ? 4096 : 2048;
    for (int counts = -maximum; counts < maximum; ++counts) {
      int16_t actual = 0;
      CHECK(t::TMP1x2::celsiusToCounts(static_cast<float>(counts) / 16.0f, extended != 0, actual).ok());
      CHECK(actual == counts); CHECK(t::TMP1x2::countsToCelsius(actual) == static_cast<float>(counts) / 16.0f);
    }
  }
  int16_t counts = 123;
  CHECK(t::TMP1x2::celsiusToCounts(-0.03125f, false, counts).ok()); CHECK(counts == -1);
  CHECK(t::TMP1x2::celsiusToCounts(0.03125f, false, counts).ok()); CHECK(counts == 1);
  counts = 123;
  CHECK(!t::TMP1x2::celsiusToCounts(128, false, counts).ok()); CHECK(counts == 123);
  CHECK(!t::TMP1x2::celsiusToCounts(std::numeric_limits<float>::infinity(), true, counts).ok()); CHECK(counts == 123);
  Device d; t::TMP1x2 driver; CHECK(driver.init(d.config()).ok());
  float fahrenheit = 0; CHECK(driver.readTemperatureFahrenheit(fahrenheit).ok()); CHECK(fahrenheit == 77);
  CHECK(driver.readTemperatureCounts(counts).ok()); CHECK(counts == 400);
  d.failAt = d.calls + 1; fahrenheit = 999;
  CHECK(!driver.readTemperatureFahrenheit(fahrenheit).ok()); CHECK(fahrenheit == 999);
  d.failAt = d.calls + 1; counts = 999;
  CHECK(!driver.readTemperatureCounts(counts).ok()); CHECK(counts == 999);
}

static void lifecycleAndCachedNames() {
  Device d; t::TMP1x2 driver;
  CHECK(driver.init().is(t::Err::NOT_BOUND)); CHECK(driver.begin().is(t::Err::NOT_BOUND));
  CHECK(driver.bind(d.config()).ok()); CHECK(d.calls == 0); CHECK(driver.init().ok());
  const auto successes = driver.totalSuccess(); CHECK(driver.begin().ok()); CHECK(driver.totalSuccess() > successes);
  CHECK(driver.getMode() == t::Mode::CONTINUOUS && driver.getConversionRate() == t::ConversionRate::HZ_4);
  CHECK(!driver.getExtendedMode() && driver.getAlertMode() == t::AlertMode::COMPARATOR);
  CHECK(driver.getAlertPolarity() == t::AlertPolarity::ACTIVE_LOW && driver.getFaultQueue() == t::FaultQueue::FAULTS_1);
  CHECK(driver.getLowThresholdC() == 75 && driver.getHighThresholdC() == 80);
  unsigned before = d.calls;
  t::SettingsSnapshot settings; CHECK(driver.getSettings(settings).ok());
  CHECK(settings.initialized && driver.getSettings().config.i2cAddress == 0x48); CHECK(d.calls == before);
  uint16_t raw = 0; CHECK(driver.readRegister16(0, raw).ok()); CHECK(raw == 0x1900);
  CHECK(driver.readConfiguration(raw).ok()); CHECK((raw & 0x1FD0) == 0x80);
  d.registers[1] ^= 0x40; CHECK(driver.readConfiguration(raw).ok()); CHECK(driver.hardwareConfigDirty());
  CHECK(driver.init().ok());
  CHECK(driver.writeRegister16(2, 0x4400).ok()); CHECK(driver.hardwareConfigDirty()); CHECK(driver.init().ok());
  t::OperationToken token = 0; before = d.calls;
  CHECK(driver.startInit(d.ms, 1000, token).inProgress()); CHECK(d.calls == before);
  CHECK(driver.getSettings(settings).ok()); CHECK(settings.initialized);
  CHECK(driver.init().is(t::Err::BUSY)); CHECK(driver.begin().is(t::Err::BUSY)); CHECK(d.calls == before);
  t::OperationResult result; CHECK(complete(driver, d, token, result));
}

static void cacheTimeProvenance() {
  Device d; t::TMP1x2 driver; CHECK(driver.begin(d.config(false)).ok());
  t::Sample sample; sample.celsius = 999;
  CHECK(driver.getLastSample(sample).is(t::Err::MEASUREMENT_NOT_READY)); CHECK(sample.celsius == 999);
  CHECK(driver.readSample(sample).ok()); CHECK(!sample.timestampValid && !sample.freshConversion);
  CHECK(!driver.sampleTimestampValid() && !driver.sampleFresh(0, 0));
  CHECK(driver.sampleAgeMs(100) == 100); // Historical epoch-relative API remains compatible.
  sample.celsius = 999; CHECK(!driver.getLastSample(sample, 0, 0).ok()); CHECK(sample.celsius == 999);
  driver.tick(0); CHECK(!driver.sampleFresh(0, 10)); // Time cannot repair an earlier unknown acquisition.
  CHECK(driver.readSample(sample).ok()); CHECK(sample.timestampValid && !sample.freshConversion);
  CHECK(driver.sampleFresh(0, 0) && driver.sampleTimestampMs() == 0);
  const auto calls = d.calls; CHECK(driver.sampleAgeMs(100) == 100); CHECK(driver.sampleAgeMs() == 0); CHECK(d.calls == calls);
  CHECK(driver.getLastSample(sample, 1, 1).ok()); sample.celsius = 999;
  CHECK(driver.getLastSample(sample, 2, 1).is(t::Err::MEASUREMENT_NOT_READY)); CHECK(sample.celsius == 999);
  CHECK(driver.getLastSample(sample, 2, UINT32_MAX).is(t::Err::INVALID_PARAM)); CHECK(sample.celsius == 999);
  CHECK(driver.writeRegisterRaw(2, 0x4400).ok()); CHECK(!driver.sampleFresh(0, 0));
  CHECK(driver.getLastSample(sample).ok()); CHECK(sample.celsius == 25);
  sample.celsius = 999; CHECK(driver.getLastSample(sample, 0, 0).is(t::Err::INVALID_CONFIG)); CHECK(sample.celsius == 999);
  CHECK(driver.recover().ok()); CHECK(!driver.hasSample());
  CHECK(driver.readSample(sample).ok()); CHECK(driver.setConversionRate(t::ConversionRate::HZ_1).ok()); CHECK(!driver.hasSample());
  d.ms = UINT32_MAX - 2; CHECK(driver.begin(d.config()).ok()); CHECK(driver.readSample(sample).ok());
  CHECK(driver.sampleAgeMs(3) == 6); CHECK(driver.sampleFresh(3, 6)); CHECK(!driver.sampleFresh(3, 5));
}

static void completedConversionProvenance() {
  Device d; t::TMP1x2 driver; auto config = d.config(); config.mode = t::Mode::SHUTDOWN;
  CHECK(driver.begin(config).ok()); CHECK(driver.startOneShot().ok()); d.ms += 35;
  t::Sample sample; CHECK(driver.tryRead(sample).ok()); CHECK(sample.freshConversion && sample.timestampValid);
  CHECK(driver.lastSample().freshConversion);
  CHECK(driver.readSample(sample).ok()); CHECK(!sample.freshConversion); // Reading the same register is not a new conversion.
  t::OperationToken token = 0; CHECK(driver.startRead(d.ms, 1000, token).inProgress());
  t::OperationResult result; CHECK(complete(driver, d, token, result));
  CHECK(result.hasSample && result.sample.freshConversion && result.sample.timestampValid);
  CHECK(driver.lastSample().freshConversion);
}

static void healthAndInvalidation() {
  Device d; t::TMP1x2 driver; auto c = d.config(false); c.offlineThreshold = 2;
  CHECK(driver.begin(c).ok()); CHECK(!driver.healthSnapshot().lastOkTimeValid);
  driver.tick(0); uint16_t raw = 0; CHECK(driver.readRegister(0, raw).ok());
  CHECK(driver.healthSnapshot().lastOkTimeValid && driver.lastOkMs() == 0);
  driver.tick(100);
  for (unsigned n = 0; n < 2; ++n) { d.failAt = d.calls + 1; CHECK(!driver.readRegister(0, raw).ok()); }
  auto prior = driver.healthSnapshot(); CHECK(prior.state == t::DriverState::OFFLINE && prior.lastErrorTimeValid);
  CHECK(prior.lastError.detail == 91 && prior.consecutiveFailures == 2);
  unsigned calls = d.calls; driver.resetStatistics();
  const auto reset = driver.healthSnapshot();
  CHECK(reset.totalSuccess == 0 && reset.totalFailures == 0 && reset.state == prior.state);
  CHECK(reset.consecutiveFailures == prior.consecutiveFailures && reset.lastError.detail == prior.lastError.detail);
  CHECK(reset.lastErrorMs == prior.lastErrorMs && reset.lastOkTimeValid == prior.lastOkTimeValid); CHECK(d.calls == calls);
  d.failAt = 0; CHECK(driver.invalidateDeviceState().ok()); CHECK(d.calls == calls);
  CHECK(driver.hardwareConfigDirty() && !driver.hasSample()); CHECK(driver.state() == t::DriverState::OFFLINE);
  CHECK(driver.recover().is(t::Err::INVALID_CONFIG)); // Fresh conversion is required, even when CONFIG already matches.
  t::OperationToken token = 0; CHECK(driver.startRecover(d.ms, 1000, token).inProgress());
  calls = d.calls; CHECK(driver.invalidateDeviceState().is(t::Err::BUSY)); CHECK(d.calls == calls);
  while (driver.operationActive()) { driver.poll(d.ms, 1); ++d.ms; }
  calls = d.calls; CHECK(driver.invalidateDeviceState().is(t::Err::BUSY)); CHECK(d.calls == calls);
  t::OperationResult result; CHECK(driver.takeResult(token, result).ok()); CHECK(result.status.ok());
  c = d.config(); c.mode = t::Mode::SHUTDOWN; CHECK(driver.begin(c).ok()); CHECK(driver.startOneShot().ok());
  calls = d.calls; CHECK(driver.invalidateDeviceState().ok()); CHECK(d.calls == calls);
  CHECK(!driver.conversionStarted() && driver.hardwareConfigDirty()); CHECK(driver.recover().ok());
  driver.unbind(); CHECK(driver.invalidateDeviceState().is(t::Err::NOT_BOUND));
}

static void registerSnapshots() {
  Device d; t::TMP1x2 driver; t::RegisterSnapshot snapshot; snapshot.rawTemperature = 0xDEAD;
  CHECK(driver.readSnapshot(snapshot).is(t::Err::NOT_INITIALIZED)); CHECK(snapshot.rawTemperature == 0xDEAD);
  CHECK(driver.begin(d.config()).ok()); const unsigned first = d.calls;
  CHECK(driver.readSnapshot(snapshot).ok()); CHECK(d.calls - first == 5);
  CHECK(snapshot.temperatureDecoded && snapshot.temperatureTrusted && !snapshot.temperature.freshConversion);
  CHECK(snapshot.configurationMatchesDesired && snapshot.thresholdsMatchDesired && snapshot.timestampValid);
  CHECK(snapshot.temperature.celsius == 25 && snapshot.lowThresholdC == 75 && snapshot.highThresholdC == 80);
  CHECK(!driver.hasSample());
  t::Sample sample; CHECK(driver.readSample(sample).ok()); d.registers[0] = 0x1A00;
  CHECK(driver.readSnapshot(snapshot).ok()); CHECK(snapshot.temperature.celsius == 26 && driver.lastSample().celsius == 25);
  d.changeMask = 0x8020; d.changeAt = d.calls + 5;
  CHECK(driver.readSnapshot(snapshot).ok()); CHECK(snapshot.temperatureTrusted);
  d.changeMask = 0x40; d.changeAt = d.calls + 5; snapshot.rawTemperature = 0xDEAD;
  CHECK(driver.readSnapshot(snapshot).is(t::Err::CONFIG_MISMATCH)); CHECK(snapshot.rawTemperature == 0xDEAD);
  CHECK(driver.hardwareConfigDirty()); CHECK(driver.recover().ok());
  CHECK(driver.writeRegisterRaw(1, d.registers[1]).ok()); CHECK(driver.readSnapshot(snapshot).ok());
  CHECK(snapshot.temperatureDecoded && !snapshot.temperatureTrusted); CHECK(driver.hardwareConfigDirty());
  CHECK(driver.recover().ok()); d.registers[0] = 0x1901;
  CHECK(driver.readSnapshot(snapshot).ok()); CHECK(snapshot.temperatureDecoded && !snapshot.temperatureTrusted);
  CHECK(snapshot.temperature.celsius == 50 && driver.hardwareConfigDirty()); // Decoded diagnostics explicitly untrusted.
  CHECK(driver.recover().ok()); d.registers[0] = 0x1902;
  CHECK(driver.readSnapshot(snapshot).ok()); CHECK(!snapshot.temperatureDecoded && !snapshot.temperatureTrusted);
  CHECK(snapshot.rawTemperature == 0x1902 && driver.hardwareConfigDirty());
}

static void snapshotFailureEvidence() {
  for (unsigned fail = 1; fail <= 5; ++fail) {
    Device d; t::TMP1x2 driver; CHECK(driver.begin(d.config()).ok());
    t::RegisterSnapshot snapshot; snapshot.rawTemperature = 0xDEAD; snapshot.rawHighThreshold = 0xBEEF;
    d.failAt = d.calls + fail;
    CHECK(driver.readSnapshot(snapshot).is(t::Err::I2C_TIMEOUT));
    CHECK(snapshot.rawTemperature == 0xDEAD && snapshot.rawHighThreshold == 0xBEEF);
  }
  for (unsigned evidence = 0; evidence < 3; ++evidence) {
    Device d; t::TMP1x2 driver; CHECK(driver.begin(d.config()).ok());
    if (evidence == 0) { d.store(static_cast<uint16_t>(d.registers[1] | 0x10U)); d.failAt = d.calls + 2; }
    if (evidence == 1) { d.registers[0] = 0x1901; d.failAt = d.calls + 3; }
    if (evidence == 2) { d.registers[2] ^= 0x100U; d.failAt = d.calls + 4; }
    t::RegisterSnapshot snapshot; snapshot.rawTemperature = 0xDEAD;
    CHECK(driver.readSnapshot(snapshot).is(t::Err::I2C_TIMEOUT)); CHECK(snapshot.rawTemperature == 0xDEAD);
    CHECK(driver.hardwareConfigDirty()); d.failAt = 0;
    if (evidence == 0) CHECK(driver.setExtendedMode(true).ok()); else CHECK(driver.recover().ok());
    t::Sample sample; CHECK(driver.readSample(sample).ok()); CHECK(sample.celsius == 25);
  }
}

int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {{"public validation and encoding", validationAndEncoding}, {"temperature unit helpers", unitConversions},
    {"lifecycle and cached API parity", lifecycleAndCachedNames}, {"sample clock provenance and age", cacheTimeProvenance},
    {"one-shot sample provenance", completedConversionProvenance}, {"health statistics and invalidation", healthAndInvalidation},
    {"live register snapshots", registerSnapshots}, {"snapshot failure evidence", snapshotFailureEvidence}};
  for (const auto& test : tests) { const int before = failures; test.run(); if (before == failures) std::printf("[PASS] %s\n", test.name); }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
