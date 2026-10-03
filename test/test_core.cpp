#include "TMP1x2/TMP1x2.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace t = TMP1x2;
static_assert(!std::is_copy_constructible<t::TMP1x2>::value, "driver owns state");
static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("[FAIL] %s:%d %s\n", __func__, __LINE__, #expr); ++failures; return; } } while (false)

// Device double, deliberately implements bus framing and silicon read-only bits.
// Fault injection can model failure after hardware accepted a write.
struct Bus {
  uint16_t regs[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  unsigned calls = 0;
  unsigned writes = 0;
  unsigned failAt = 0;
  unsigned corruptAt = 0;
  bool failAll = false;
  bool acceptFailedWrite = false;
  bool acceptFailedMsb = false;
  bool conversionCompletes = true;
  bool clockAdvances = true;
  uint32_t ms = 0;
  uint32_t shotAt = 0;
  bool shot = false;
  uint32_t shutdownAt = 0;
  bool prematureEmChange = false;
  uint32_t observedTimeout = 0;
  t::Err error = t::Err::I2C_TIMEOUT;

  static uint32_t clock(void* p) { return static_cast<Bus*>(p)->ms; }
  static void yield(void* p) { auto& b = *static_cast<Bus*>(p); if (b.clockAdvances) ++b.ms; }
  t::Status result() const { return t::Status::Error(error, "injected", 731); }
  bool failed() const { return failAll || (failAt != 0 && calls == failAt); }
  void store(uint8_t reg, uint16_t value) {
    if (reg == 1) {
      // TI-reported behavior: EM marker can change before sample payload.
      if (((regs[1] ^ value) & 0x0010U) != 0)
        regs[0] = static_cast<uint16_t>((regs[0] & 0xFFFEU) | ((value & 0x0010U) != 0 ? 1U : 0U));
      if (((regs[1] ^ value) & 0x0010U) != 0 &&
          ((regs[1] & 0x0100U) == 0 || static_cast<uint32_t>(ms - shutdownAt) < 35U))
        prematureEmChange = true;
      if ((regs[1] & 0x0100U) == 0 && (value & 0x0100U) != 0) shutdownAt = ms;
      regs[1] = static_cast<uint16_t>((value & 0x1FD0U) | 0x6020U);
      if ((value & 0x8100U) == 0x8100U) { shot = true; shotAt = ms; }
      else if ((value & 0x0100U) != 0) { regs[1] |= 0x8000U; }
      else { shot = false; }
    } else { regs[reg] = value; }
  }
  static t::Status write(uint8_t addr, const uint8_t* tx, size_t len, uint32_t timeout, void* p) {
    auto& b = *static_cast<Bus*>(p); ++b.calls; ++b.writes; b.observedTimeout = timeout;
    if (addr < 0x48 || addr > 0x4B || len != 3 || tx[0] < 1 || tx[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "invalid framing");
    if (!b.failed() || b.acceptFailedWrite)
      b.store(tx[0], static_cast<uint16_t>((static_cast<uint16_t>(tx[1]) << 8U) | tx[2]));
    else if (b.acceptFailedMsb)
      // TI updates registers byte by byte. A failed transfer can apply the
      // MSB (including OS/SD) while preserving the old LSB (including EM).
      b.store(tx[0], static_cast<uint16_t>((static_cast<uint16_t>(tx[1]) << 8U) |
                                         (b.regs[tx[0]] & 0x00FFU)));
    return b.failed() ? b.result() : t::Status::Ok();
  }
  static t::Status read(uint8_t addr, const uint8_t* tx, size_t len, uint8_t* rx, size_t n, uint32_t timeout, void* p) {
    auto& b = *static_cast<Bus*>(p); ++b.calls; b.observedTimeout = timeout;
    if (addr < 0x48 || addr > 0x4B || len != 1 || n != 2 || tx[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "invalid framing");
    if (b.failed()) { rx[0] = 0xFF; rx[1] = 0xFF; return b.result(); }
    if (b.shot && b.conversionCompletes && static_cast<uint32_t>(b.ms - b.shotAt) >= 35) {
      b.shot = false; b.regs[1] |= 0x8000U;
      b.regs[0] = (b.regs[1] & 0x10U) != 0 ? 0x0C81 : 0x1900;
    }
    auto value = b.regs[tx[0]];
    if (b.corruptAt == b.calls) value ^= 0x100U;
    rx[0] = static_cast<uint8_t>(value >> 8U); rx[1] = static_cast<uint8_t>(value);
    return t::Status::Ok();
  }
  t::Config config() {
    t::Config c; c.i2cWrite = write; c.i2cWriteRead = read; c.i2cUser = this;
    c.nowMs = clock; c.cooperativeYield = yield; c.timeUser = this; return c;
  }
};

static void decoding() {
  const uint16_t words[] = {0x0000, 0x1900, 0xFFF0, 0xE700, 0x7FF0, 0x8000, 0x4B01, 0xFFF9, 0x8001};
  const float expected[] = {0, 25, -0.0625f, -25, 127.9375f, -128, 150, -0.0625f, -256};
  for (unsigned i = 0; i < sizeof(words)/sizeof(words[0]); ++i) {
    t::Sample sample; CHECK(t::TMP1x2::decodeTemperature(words[i], sample).ok());
    CHECK(sample.celsius == expected[i]); CHECK(sample.raw == words[i]);
  }
  // Exhaust the signed normal/extended code space, independent expected math.
  for (int extended = 0; extended <= 1; ++extended) {
    const int limit = extended ? 4096 : 2048;
    for (int code = -limit; code < limit; ++code) {
      const unsigned bits = static_cast<unsigned>(code) & (extended ? 8191U : 4095U);
      const auto word = static_cast<uint16_t>((bits << (extended ? 3U : 4U)) | static_cast<unsigned>(extended));
      t::Sample sample; CHECK(t::TMP1x2::decodeTemperature(word, sample).ok());
      CHECK(sample.counts == code); CHECK(sample.celsius == static_cast<float>(code) / 16.0f);
    }
  }
}
static void thresholds() {
  uint16_t out = 0x1234;
  CHECK(t::TMP1x2::encodeThreshold(-25, false, out).ok()); CHECK(out == 0xE700);
  CHECK(t::TMP1x2::encodeThreshold(150, true, out).ok()); CHECK(out == 0x4B00);
  CHECK(t::TMP1x2::decodeThreshold(out, true) == 150);
  CHECK(t::TMP1x2::encodeThreshold(0.03125f, false, out).ok()); CHECK(out == 0x10);
  CHECK(t::TMP1x2::encodeThreshold(-0.03125f, false, out).ok()); CHECK(out == 0xFFF0);
  out = 0x1234;
  CHECK(!t::TMP1x2::encodeThreshold(128, false, out).ok()); CHECK(out == 0x1234);
  CHECK(!t::TMP1x2::encodeThreshold(std::numeric_limits<float>::quiet_NaN(), true, out).ok());
  CHECK(!t::TMP1x2::encodeThreshold(std::numeric_limits<float>::infinity(), true, out).ok());
}
static void validation() {
  Bus b; t::TMP1x2 d; auto c = b.config(); c.i2cAddress = 0x40;
  CHECK(!d.begin(c).ok()); CHECK(b.calls == 0);
  c = b.config(); c.i2cWrite = nullptr; CHECK(!d.begin(c).ok()); CHECK(b.calls == 0);
  c = b.config(); c.mode = static_cast<t::Mode>(9); CHECK(!d.begin(c).ok()); CHECK(b.calls == 0);
  c = b.config(); c.lowThresholdC = 90; CHECK(!d.begin(c).ok()); CHECK(b.calls == 0);
  c = b.config(); c.i2cTimeoutMs = 0; CHECK(!d.begin(c).ok()); CHECK(b.calls == 0);
  CHECK(d.begin(b.config()).ok()); const unsigned calls = b.calls;
  CHECK(!d.setFaultQueue(static_cast<t::FaultQueue>(7)).ok()); CHECK(b.calls == calls);
  CHECK(!d.writeRegister(0, 0).ok()); CHECK(b.calls == calls);
  uint16_t raw = 123; CHECK(!d.readRegister(4, raw).ok()); CHECK(raw == 123);
  CHECK(d.totalFailures() == 0);
}
static void lifecycle() {
  Bus b; t::TMP1x2 d; CHECK(d.state() == t::DriverState::UNINIT);
  t::Sample sample; CHECK(!d.readSample(sample).ok()); CHECK(b.calls == 0);
  CHECK(d.begin(b.config()).ok()); CHECK(d.isInitialized()); CHECK(d.state() == t::DriverState::READY);
  CHECK((b.regs[1] & 0x1FD0) == 0x0080); CHECK(b.regs[2] == 0x4B00); CHECK(b.regs[3] == 0x5000);
  CHECK(d.readSample(sample).ok()); CHECK(sample.celsius == 25); CHECK(b.observedTimeout == 50);
  const unsigned calls = b.calls; d.end(); CHECK(b.calls == calls); CHECK(!d.isInitialized());
  CHECK(d.state() == t::DriverState::UNINIT);
}
static void health() {
  Bus b; t::TMP1x2 d; auto c = b.config(); c.offlineThreshold = 3; CHECK(d.begin(c).ok());
  const auto success = d.totalSuccess(); CHECK(d.probe().ok()); CHECK(d.totalSuccess() == success);
  b.failAll = true; uint16_t raw = 17;
  CHECK(!d.readRegister(0, raw).ok()); CHECK(raw == 17); CHECK(d.state() == t::DriverState::DEGRADED);
  CHECK(!d.readRegister(0, raw).ok()); CHECK(!d.readRegister(0, raw).ok());
  CHECK(d.state() == t::DriverState::OFFLINE); CHECK(d.totalFailures() == 3); CHECK(d.lastError().detail == 731);
  const auto failed = d.totalFailures(); CHECK(!d.probe().ok()); CHECK(d.totalFailures() == failed);
  b.failAll = false; CHECK(d.readRegister(0, raw).ok()); CHECK(d.state() == t::DriverState::READY);
  CHECK(d.consecutiveFailures() == 0);
}
static void dirtyRecovery() {
  Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok());
  b.failAt = b.calls + 2; b.acceptFailedWrite = true;
  CHECK(!d.setThresholds(-10, 30).ok()); CHECK(d.hardwareConfigDirty());
  t::Sample sample; sample.celsius = 999; CHECK(!d.readSample(sample).ok()); CHECK(sample.celsius == 999);
  b.failAt = 0; CHECK(d.recover().ok()); CHECK(!d.hardwareConfigDirty());
  float low = 0, high = 0; CHECK(d.readThresholds(low, high).ok()); CHECK(low == -10 && high == 30);
  CHECK(d.writeRegister(1, 0x6190).ok()); CHECK(d.hardwareConfigDirty());
  CHECK(d.recover().ok()); CHECK(!d.hardwareConfigDirty());
  b.regs[2] ^= 0x100; CHECK(!d.verifyConfiguration().ok()); CHECK(d.hardwareConfigDirty());
  CHECK(d.recover().ok());
}
static void oneShot() {
  Bus b; t::TMP1x2 d; auto c = b.config(); c.mode = t::Mode::SHUTDOWN;
  CHECK(d.begin(c).ok()); t::Sample s; CHECK(d.tryRead(s).is(t::Err::MEASUREMENT_NOT_READY));
  CHECK(d.startOneShot().ok()); CHECK(d.startOneShot().is(t::Err::BUSY));
  const uint32_t started = b.ms;
  bool ready = true; CHECK(d.isConversionReady(ready).ok()); CHECK(!ready);
  b.ms = started + 34U; CHECK(d.tryRead(s).is(t::Err::MEASUREMENT_NOT_READY));
  b.ms = started + 35U; CHECK(d.tryRead(s).ok()); CHECK(s.celsius == 25);
  CHECK(d.tryRead(s).is(t::Err::MEASUREMENT_NOT_READY));
  CHECK(d.readBlocking(s, 100).ok()); CHECK(s.celsius == 25);
  CHECK(d.readBlocking(s, 1).is(t::Err::TIMEOUT));
}
static void conversionRequiresHardwareCompletion() {
  Bus b; t::TMP1x2 d; auto c = b.config(); c.mode = t::Mode::SHUTDOWN;
  CHECK(d.begin(c).ok()); b.conversionCompletes = false;
  CHECK(d.startOneShot().ok()); b.ms += 1000U;
  t::Sample sample; sample.celsius = 999;
  const auto failuresBefore = d.totalFailures();
  CHECK(d.tryRead(sample).is(t::Err::MEASUREMENT_NOT_READY));
  CHECK(sample.celsius == 999 && d.conversionStarted());
  CHECK(!d.conversionReady() && !d.hardwareConfigDirty());
  CHECK(d.totalFailures() == failuresBefore);
  b.conversionCompletes = true;
  CHECK(d.tryRead(sample).ok() && sample.freshConversion);

  // Elapsed settling time must not certify a new EM payload either. A readable
  // CONFIG with OS still busy is a protocol timeout, not a transport failure.
  b.conversionCompletes = false;
  CHECK(d.setExtendedMode(true).is(t::Err::TIMEOUT));
  CHECK(d.hardwareConfigDirty() && d.hardwareConfigDirtyError().is(t::Err::TIMEOUT));
  CHECK(d.totalFailures() == failuresBefore);
  sample.celsius = 999;
  CHECK(d.readSample(sample).is(t::Err::INVALID_CONFIG) && sample.celsius == 999);
  b.conversionCompletes = true;
  CHECK(d.recover().ok()); CHECK(d.readSample(sample).ok());
  CHECK(sample.celsius == 25 && sample.extendedMode);
}
static void verificationPreservesMismatchEvidence() {
  for (unsigned extended = 0; extended < 2; ++extended) {
    Bus b; t::TMP1x2 d; auto c = b.config(); c.extendedMode = extended != 0;
    CHECK(d.begin(c).ok());
    b.regs[2] ^= 0x100U;
    b.failAt = b.calls + 3U; // CONFIG and changed TLOW succeed, THIGH fails.
    const auto failuresBefore = d.totalFailures();
    const auto successesBefore = d.totalSuccess();
    CHECK(d.verifyConfiguration().is(t::Err::I2C_TIMEOUT));
    CHECK(d.hardwareConfigDirty());
    CHECK(d.hardwareConfigDirtyError().is(t::Err::CONFIG_MISMATCH));
    CHECK(d.hardwareConfigDirtyError().detail == b.regs[2]);
    CHECK(d.lastError().is(t::Err::I2C_TIMEOUT));
    CHECK(d.totalFailures() == failuresBefore + 1U);
    CHECK(d.totalSuccess() == successesBefore + 2U);
    b.failAt = 0; CHECK(d.recover().ok()); CHECK(!d.hardwareConfigDirty());
  }
}
static void wraparound() {
  Bus b; b.ms = UINT32_MAX - 10U; t::TMP1x2 d; auto c = b.config(); c.mode = t::Mode::SHUTDOWN;
  CHECK(d.begin(c).ok()); CHECK(d.startOneShot().ok()); b.ms += 35U;
  t::Sample s; CHECK(d.tryRead(s).ok()); CHECK(s.timestampMs == b.ms);
}
static void extendedAndAlert() {
  Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok()); CHECK(d.setExtendedMode(true).ok());
  CHECK(!b.prematureEmChange); CHECK(b.ms >= 70U);
  CHECK(b.regs[2] == 0x2580); CHECK(b.regs[3] == 0x2800);
  b.regs[0] = 0x4B01; t::Sample s; CHECK(d.readSample(s).ok()); CHECK(s.celsius == 150 && s.extendedMode);
  CHECK(d.setThresholds(140, 150).ok()); CHECK(!d.setExtendedMode(false).ok());
  for (unsigned polarity = 0; polarity != 2; ++polarity) {
    for (unsigned al = 0; al != 2; ++al) {
      const auto raw = static_cast<uint16_t>(0x6000U | (polarity << 10U) | (al << 5U));
      const auto info = t::TMP1x2::decodeConfiguration(raw);
      CHECK(info.valid); CHECK(info.alertActive == (al == polarity));
    }
  }
}
static void protocolAndClockGuards() {
  Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok());
  b.regs[0] = 0x1902; t::Sample sample; sample.celsius = 999;
  const auto success = d.totalSuccess(), failuresBefore = d.totalFailures();
  CHECK(d.readSample(sample).is(t::Err::CONFIG_MISMATCH)); CHECK(sample.celsius == 999);
  CHECK(d.hardwareConfigDirty()); CHECK(d.totalSuccess() == success + 1U); CHECK(d.totalFailures() == failuresBefore);
  CHECK(d.recover().ok());
  b.failAll = true; b.error = t::Err::IN_PROGRESS;
  uint16_t raw = 99; const auto status = d.readRegister(0, raw);
  CHECK(!status.ok() && !status.inProgress()); CHECK(status.detail == 731); CHECK(raw == 99);
  b.failAll = false; CHECK(d.recover().ok());
  auto noClock = b.config(); noClock.nowMs = nullptr; noClock.cooperativeYield = nullptr;
  CHECK(d.begin(noClock).ok()); const auto calls = b.writes;
  CHECK(!d.setExtendedMode(true).ok()); CHECK(b.writes == calls);
  auto stoppedClock = b.config(); stoppedClock.mode = t::Mode::SHUTDOWN;
  b.clockAdvances = false;
  CHECK(!d.begin(stoppedClock).ok()); CHECK(d.hardwareConfigDirty());
  b.clockAdvances = true;
  CHECK(d.begin(d.getConfig()).ok()); CHECK(!d.hardwareConfigDirty());
}
static void formatRecovery() {
  for (unsigned rebind = 0; rebind < 2; ++rebind) {
    Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok());
    // Fail TLOW after old shutdown and EM write reached the device.
    b.failAt = b.calls + 4U;
    CHECK(!d.setExtendedMode(true).ok()); CHECK(d.hardwareConfigDirty());
    CHECK((b.regs[1] & 0x10U) != 0); CHECK(b.regs[0] == 0x1901);
    b.failAt = 0; CHECK((rebind != 0 ? d.begin(d.getConfig()) : d.recover()).ok());
    t::Sample sample; CHECK(d.readSample(sample).ok()); CHECK(sample.celsius == 25.0f);
    CHECK(!d.hardwareConfigDirty());
  }
}
static void observedFormatRecovery() {
  // Reading changed EM is lasting evidence that TEMP can have a new marker
  // with an old payload. Adopting that EM must still complete a fresh sample.
  for (unsigned observer = 0; observer < 5; ++observer) {
    for (unsigned restore = 0; restore < 2; ++restore) {
      Bus b; t::TMP1x2 d; auto c = b.config();
      if (observer >= 3) c.mode = t::Mode::SHUTDOWN;
      CHECK(d.begin(c).ok());
      if (observer == 4) CHECK(d.startOneShot().ok());
      b.store(1, static_cast<uint16_t>(b.regs[1] | 0x10U));
      // Keep the deliberate marker/payload mismatch observable for ready polls.
      b.shot = false;
      if (observer == 0) {
        t::ConfigurationInfo info; CHECK(d.readConfiguration(info).ok());
      } else if (observer == 1) {
        float low = 0, high = 0; CHECK(d.readThresholds(low, high).ok());
      } else if (observer == 2) {
        CHECK(d.verifyConfiguration().is(t::Err::CONFIG_MISMATCH));
      } else if (observer == 3) {
        CHECK(d.startOneShot().is(t::Err::CONFIG_MISMATCH));
      } else {
        b.ms += 35U; bool ready = false;
        CHECK(d.isConversionReady(ready).is(t::Err::CONFIG_MISMATCH));
      }
      CHECK(d.hardwareConfigDirty());
      if (restore != 0) {
        // A second outside change restores desired EM, but leaves a payload
        // produced in the other format; matching CONFIG alone cannot prove it.
        b.regs[0] = 0x0C81;
        b.store(1, static_cast<uint16_t>(b.regs[1] & ~0x10U));
        CHECK(d.recover().ok());
      } else {
        CHECK(d.setExtendedMode(true).ok());
      }
      t::Sample sample; CHECK(d.readSample(sample).ok());
      CHECK(sample.celsius == 25.0f); CHECK(!d.hardwareConfigDirty());
    }
  }
}
static void sameFormatMismatchWithoutClock() {
  Bus b; t::TMP1x2 d; auto c = b.config();
  c.nowMs = nullptr; c.cooperativeYield = nullptr;
  CHECK(d.begin(c).ok());
  b.regs[1] ^= 0x40U;
  t::ConfigurationInfo info; CHECK(d.readConfiguration(info).ok());
  CHECK(d.hardwareConfigDirty()); CHECK(d.recover().ok());
  b.regs[2] ^= 0x100U;
  CHECK(d.verifyConfiguration().is(t::Err::CONFIG_MISMATCH));
  CHECK(d.recover().ok()); CHECK(!d.hardwareConfigDirty());
}
static void observedTemperatureFormatRecovery() {
  for (unsigned lifecycle = 0; lifecycle < 4; ++lifecycle) {
    Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok());
    b.store(1, static_cast<uint16_t>(b.regs[1] | 0x10U));
    if (lifecycle == 3) b.regs[0] |= 2U; // Reserved-bit failure cannot erase EM evidence.
    t::Sample sample; sample.celsius = 999.0f;
    CHECK(d.readSample(sample).is(lifecycle == 3 ? t::Err::CONFIG_MISMATCH : t::Err::MEASUREMENT_NOT_READY));
    CHECK(sample.celsius == 999.0f); CHECK(d.hardwareConfigDirty());
    CHECK(d.hardwareConfigDirtyError().is(t::Err::CONFIG_MISMATCH));
    CHECK(d.readSample(sample).is(t::Err::INVALID_CONFIG));
    if (lifecycle == 0 || lifecycle == 3) {
      CHECK(d.setExtendedMode(true).ok());
    } else {
      auto c = d.getConfig(); c.extendedMode = true;
      if (lifecycle == 1) {
        d.end(); CHECK(d.bind(c).ok()); CHECK(d.recover().ok());
      } else {
        CHECK(d.begin(c).ok());
      }
    }
    CHECK(d.readSample(sample).ok()); CHECK(sample.celsius == 25.0f);
    CHECK(!d.hardwareConfigDirty());
  }
}
static void interruptedFormatObservation() {
  for (unsigned observer = 0; observer < 3; ++observer) {
    Bus b; t::TMP1x2 d; CHECK(d.begin(b.config()).ok());
    b.store(1, static_cast<uint16_t>(b.regs[1] | 0x10U));
    b.failAt = b.calls + (observer == 1 ? 3U : 2U);
    if (observer < 2) {
      float low = 999.0f, high = 999.0f;
      CHECK(d.readThresholds(low, high).is(t::Err::I2C_TIMEOUT));
      CHECK(low == 999.0f && high == 999.0f);
    } else {
      CHECK(d.recover().is(t::Err::I2C_TIMEOUT));
    }
    b.failAt = 0;
    CHECK(d.setExtendedMode(true).ok());
    t::Sample sample; CHECK(d.readSample(sample).ok());
    CHECK(sample.celsius == 25.0f); CHECK(!d.hardwareConfigDirty());
  }
}
static void formatPreconditionEvidence() {
  Bus b; t::TMP1x2 d; auto c = b.config();
  c.nowMs = nullptr; c.cooperativeYield = nullptr;
  CHECK(d.begin(c).ok());
  b.store(1, static_cast<uint16_t>(b.regs[1] | 0x10U));
  const unsigned writes = b.writes;
  CHECK(d.setConversionRate(t::ConversionRate::HZ_1).is(t::Err::INVALID_CONFIG));
  CHECK(d.hardwareConfigDirty()); CHECK(b.writes == writes);
  CHECK(d.setExtendedMode(true).is(t::Err::INVALID_CONFIG));
  CHECK(d.hardwareConfigDirty()); CHECK(b.writes == writes);
  c = d.getConfig(); c.nowMs = Bus::clock; c.cooperativeYield = Bus::yield;
  CHECK(d.begin(c).ok());
  t::Sample sample; CHECK(d.readSample(sample).ok());
  CHECK(sample.celsius == 25.0f); CHECK(!d.hardwareConfigDirty());
}
static void formatFailureMatrix() {
  for (unsigned initialExtended = 0; initialExtended < 2; ++initialExtended) {
    Bus baseline; t::TMP1x2 good; auto c = baseline.config();
    c.extendedMode = initialExtended != 0; CHECK(good.begin(c).ok());
    const unsigned before = baseline.calls;
    CHECK(good.setExtendedMode(initialExtended == 0).ok());
    const unsigned count = baseline.calls - before;
    for (unsigned at = 1; at <= count; ++at) {
      for (unsigned accepted = 0; accepted < 3; ++accepted) {
        Bus b; t::TMP1x2 d; c = b.config();
        c.extendedMode = initialExtended != 0; CHECK(d.begin(c).ok());
        b.failAt = b.calls + at;
        b.acceptFailedWrite = accepted == 2;
        b.acceptFailedMsb = accepted == 1;
        CHECK(d.setExtendedMode(initialExtended == 0).is(t::Err::I2C_TIMEOUT));
        CHECK(d.hardwareConfigDirty()); CHECK(d.totalFailures() == 1U);
        b.failAt = 0; CHECK(d.recover().ok());
        t::Sample sample; CHECK(d.readSample(sample).ok());
        CHECK(sample.extendedMode == (initialExtended == 0));
        CHECK(sample.celsius == 25.0f); CHECK(!b.prematureEmChange);
        CHECK(!d.hardwareConfigDirty());
      }
    }
  }
}
static void thresholdPartialWriteFailureMatrix() {
  for (unsigned extended = 0; extended < 2; ++extended) {
    Bus baseline; t::TMP1x2 good; auto c = baseline.config(); c.extendedMode = extended != 0;
    CHECK(good.begin(c).ok()); const unsigned before = baseline.calls;
    CHECK(good.setThresholds(-10.1875f, 30.3125f).ok());
    const unsigned count = baseline.calls - before;
    for (unsigned at = 1; at <= count; ++at) {
      Bus b; t::TMP1x2 d; c = b.config(); c.extendedMode = extended != 0;
      CHECK(d.begin(c).ok());
      b.failAt = b.calls + at; b.acceptFailedMsb = true;
      CHECK(d.setThresholds(-10.1875f, 30.3125f).is(t::Err::I2C_TIMEOUT));
      CHECK(d.hardwareConfigDirty()); CHECK(d.totalFailures() == 1U);
      b.failAt = 0; CHECK(d.recover().ok()); CHECK(!d.hardwareConfigDirty());
      float low = 0, high = 0;
      CHECK(d.readThresholds(low, high).ok()); CHECK(low == -10.1875f && high == 30.3125f);
    }
  }
}
static void failureMatrix() {
  Bus baseline; t::TMP1x2 good; CHECK(good.begin(baseline.config()).ok()); const unsigned count = baseline.calls;
  for (unsigned at = 1; at <= count; ++at) {
    Bus b; t::TMP1x2 d; b.failAt = at; b.acceptFailedWrite = true;
    CHECK(!d.begin(b.config()).ok()); CHECK(!d.isInitialized()); CHECK(d.state() == t::DriverState::UNINIT);
    b.failAt = 0; CHECK(d.recover().ok()); CHECK(d.isInitialized()); CHECK(!d.hardwareConfigDirty());
  }
}
int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {{"decoding", decoding}, {"thresholds", thresholds}, {"validation", validation},
    {"lifecycle", lifecycle}, {"health", health}, {"dirty recovery", dirtyRecovery}, {"one-shot", oneShot},
    {"wraparound", wraparound}, {"extended and alert", extendedAndAlert}, {"initialization failure matrix", failureMatrix},
    {"protocol and clock guards", protocolAndClockGuards}, {"partial format recovery", formatRecovery},
    {"observed format recovery", observedFormatRecovery}, {"same-format mismatch without clock", sameFormatMismatchWithoutClock},
    {"observed temperature format recovery", observedTemperatureFormatRecovery},
    {"interrupted format observation", interruptedFormatObservation},
    {"format precondition evidence", formatPreconditionEvidence},
    {"format transition failure matrix", formatFailureMatrix},
    {"threshold partial-byte failure matrix", thresholdPartialWriteFailureMatrix},
    {"conversion requires hardware completion", conversionRequiresHardwareCompletion},
    {"verification mismatch evidence", verificationPreservesMismatchEvidence}};
  for (const auto& test : tests) { const int before = failures; test.run(); if (before == failures) std::printf("[PASS] %s\n", test.name); }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
