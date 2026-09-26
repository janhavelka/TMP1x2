#include "TMP1x2/TMP1x2.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace t = TMP1x2;
static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("[FAIL] %s:%d %s\n", __func__, __LINE__, #expr); ++failures; return; } } while (false)

// Independent wire/device model: old-format payload survives an EM write until
// a new one-shot completes. Time advances only when the owner or transport does.
struct Device {
  struct Transfer {
    bool write = false;
    uint8_t reg = 0;
    uint16_t value = 0;
    uint32_t timeout = 0;
    uint32_t at = 0;
  };
  std::array<Transfer, 1024> history{};
  uint16_t regs[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  unsigned calls = 0;
  unsigned writes = 0;
  unsigned failAt = 0;
  unsigned corruptAt = 0;
  uint16_t corruptMask = 0x0100;
  bool acceptFailedWrite = false;
  bool prematureFormat = false;
  bool shotPending = false;
  bool neverReady = false;
  uint32_t ms = 0;
  uint32_t shutdownAt = 0;
  uint32_t shotAt = 0;
  uint32_t callbackDuration = 0;
  unsigned yields = 0;
  t::Err failure = t::Err::I2C_TIMEOUT;

  static uint32_t clock(void* user) { return static_cast<Device*>(user)->ms; }
  static void yield(void* user) { auto& d = *static_cast<Device*>(user); ++d.yields; ++d.ms; }

  void settle() {
    if (shotPending && !neverReady && static_cast<uint32_t>(ms - shotAt) >= 35U) {
      shotPending = false;
      regs[1] |= 0x8000U;
      regs[0] = (regs[1] & 0x0010U) != 0 ? 0x0C81U : 0x1900U;
    }
  }

  void store(uint8_t reg, uint16_t value) {
    if (reg != 1) { regs[reg] = value; return; }
    const bool changesFormat = ((regs[1] ^ value) & 0x0010U) != 0;
    if (changesFormat) {
      if ((regs[1] & 0x0100U) == 0 || static_cast<uint32_t>(ms - shutdownAt) < 35U)
        prematureFormat = true;
      // The marker alone deliberately lies about the payload encoding.
      regs[0] = static_cast<uint16_t>((regs[0] & 0xFFFEU) | ((value & 0x0010U) != 0 ? 1U : 0U));
    }
    if ((regs[1] & 0x0100U) == 0 && (value & 0x0100U) != 0) shutdownAt = ms;
    regs[1] = static_cast<uint16_t>((value & 0x1FD0U) | 0x6020U);
    if ((value & 0x8100U) == 0x8100U) { shotPending = true; shotAt = ms; }
    else if ((value & 0x0100U) != 0) { if (!shotPending) regs[1] |= 0x8000U; }
    else { shotPending = false; }
  }

  t::Status finish(uint32_t timeout) {
    ms += std::min(callbackDuration, timeout);
    if (calls == failAt || callbackDuration > timeout)
      return t::Status::Error(failure, "injected transport", 731);
    return t::Status::Ok();
  }

  static t::Status write(uint8_t address, const uint8_t* data, size_t length,
                         uint32_t timeout, void* user) {
    auto& d = *static_cast<Device*>(user);
    if (address != 0x48 || length != 3 || data[0] < 1 || data[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "bad write framing");
    ++d.calls; ++d.writes;
    const uint16_t word = static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8U) | data[2]);
    if (d.calls <= d.history.size()) d.history[d.calls - 1U] = {true, data[0], word, timeout, d.ms};
    if (d.calls != d.failAt || d.acceptFailedWrite) d.store(data[0], word);
    return d.finish(timeout);
  }

  static t::Status read(uint8_t address, const uint8_t* data, size_t length,
                        uint8_t* output, size_t count, uint32_t timeout, void* user) {
    auto& d = *static_cast<Device*>(user);
    if (address != 0x48 || length != 1 || count != 2 || data[0] > 3 || timeout == 0)
      return t::Status::Error(t::Err::INVALID_PARAM, "bad read framing");
    ++d.calls; d.settle();
    auto word = d.regs[data[0]];
    if (d.calls == d.corruptAt) word ^= d.corruptMask;
    if (d.calls <= d.history.size()) d.history[d.calls - 1U] = {false, data[0], word, timeout, d.ms};
    output[0] = static_cast<uint8_t>(word >> 8U); output[1] = static_cast<uint8_t>(word);
    return d.finish(timeout);
  }

  t::Config config(bool withClock = false) {
    t::Config result;
    result.i2cWrite = write; result.i2cWriteRead = read; result.i2cUser = this;
    result.timeUser = this;
    result.nowMs = withClock ? clock : nullptr;
    result.cooperativeYield = yield;
    return result;
  }
};

static t::PollResult runToTerminal(t::TMP1x2& driver, Device& device, uint8_t budget = 1) {
  for (unsigned step = 0; step < 2000; ++step) {
    const unsigned before = device.calls;
    const auto result = driver.poll(device.ms, budget);
    if (device.calls - before > budget || device.calls - before != result.transfers) {
      std::printf("[FAIL] runToTerminal transfer accounting: calls=%u reported=%u budget=%u\n",
                  device.calls - before, static_cast<unsigned>(result.transfers), static_cast<unsigned>(budget));
      ++failures;
      return result;
    }
    if (result.done) return result;
    ++device.ms;
  }
  std::puts("[FAIL] runToTerminal operation never finished"); ++failures;
  return {};
}

static bool initialize(t::TMP1x2& driver, Device& device, t::Config config) {
  t::OperationToken token = 0;
  if (!driver.bind(config).ok() || !driver.startInitialize(device.ms, 1000, token).inProgress()) return false;
  if (!runToTerminal(driver, device).status.ok()) return false;
  t::OperationResult result;
  return driver.takeResult(token, result).ok() && result.status.ok() && driver.isInitialized();
}

static void checkExcludedApis(t::TMP1x2& driver, Device& device) {
  const unsigned before = device.calls;
  const auto config = driver.getConfig();
  t::Sample sample; sample.celsius = 987.0f;
  t::ConfigurationInfo info; info.raw = 0x1234;
  uint16_t word = 0x1234; float low = 321.0f, high = 456.0f; bool ready = true;
  CHECK(driver.bind(config).is(t::Err::BUSY));
  CHECK(driver.begin(config).is(t::Err::BUSY));
  CHECK(driver.recover().is(t::Err::BUSY));
  CHECK(driver.shutdown().is(t::Err::BUSY));
  CHECK(driver.probe().is(t::Err::BUSY));
  CHECK(driver.readSample(sample).is(t::Err::BUSY)); CHECK(sample.celsius == 987.0f);
  CHECK(driver.tryRead(sample).is(t::Err::BUSY)); CHECK(sample.celsius == 987.0f);
  CHECK(driver.readBlocking(sample, 10).is(t::Err::BUSY)); CHECK(sample.celsius == 987.0f);
  CHECK(driver.readTemperature(low).is(t::Err::BUSY)); CHECK(low == 321.0f);
  CHECK(driver.readConfiguration(info).is(t::Err::BUSY)); CHECK(info.raw == 0x1234);
  CHECK(driver.readThresholds(low, high).is(t::Err::BUSY)); CHECK(low == 321.0f && high == 456.0f);
  CHECK(driver.verifyConfiguration().is(t::Err::BUSY));
  CHECK(driver.startOneShot().is(t::Err::BUSY));
  CHECK(driver.isConversionReady(ready).is(t::Err::BUSY)); CHECK(ready);
  CHECK(driver.readRegister(0, word).is(t::Err::BUSY)); CHECK(word == 0x1234);
  CHECK(driver.readRegisterRaw(0, word).is(t::Err::BUSY)); CHECK(word == 0x1234);
  CHECK(driver.writeRegister(1, 0x6100).is(t::Err::BUSY));
  CHECK(driver.writeRegisterRaw(1, 0x6100).is(t::Err::BUSY));
  CHECK(driver.setMode(t::Mode::SHUTDOWN).is(t::Err::BUSY));
  CHECK(driver.setConversionRate(t::ConversionRate::HZ_1).is(t::Err::BUSY));
  CHECK(driver.setExtendedMode(true).is(t::Err::BUSY));
  CHECK(driver.setAlertMode(t::AlertMode::INTERRUPT_MODE).is(t::Err::BUSY));
  CHECK(driver.setAlertPolarity(t::AlertPolarity::ACTIVE_HIGH).is(t::Err::BUSY));
  CHECK(driver.setFaultQueue(t::FaultQueue::FAULTS_2).is(t::Err::BUSY));
  CHECK(driver.setThresholds(10, 20).is(t::Err::BUSY));
  driver.tick(device.ms);
  CHECK(device.calls == before);
}

static void admissionAndResults() {
  Device device; t::TMP1x2 driver; t::OperationToken token = 4321;
  CHECK(driver.startInitialize(0, 100, token).is(t::Err::NOT_BOUND)); CHECK(token == 4321);
  CHECK(driver.bind(device.config()).ok()); CHECK(device.calls == 0);
  CHECK(driver.startInitialize(10, 0, token).is(t::Err::INVALID_PARAM)); CHECK(token == 4321);
  CHECK(driver.startInitialize(10, 0x80000000U, token).is(t::Err::INVALID_PARAM)); CHECK(token == 4321);
  device.ms = 10;
  CHECK(driver.startInitialize(device.ms, 1000, token).inProgress()); CHECK(token != 0);
  CHECK(device.calls == 0); CHECK(driver.operationActive()); CHECK(!driver.resultPending());
  const auto initial = driver.getOperationSnapshot();
  CHECK(initial.token == token && initial.kind == t::OperationKind::INITIALIZE && initial.active);
  t::OperationResult unavailable; unavailable.token = 9876;
  CHECK(driver.takeResult(token, unavailable).is(t::Err::RESULT_NOT_AVAILABLE)); CHECK(unavailable.token == 9876);
  t::OperationToken rejected = 7654;
  CHECK(driver.startRecover(device.ms, 1000, rejected).is(t::Err::BUSY)); CHECK(rejected == 7654);
  checkExcludedApis(driver, device);
  const auto zero = driver.poll(device.ms, 0);
  CHECK(zero.status.inProgress() && !zero.done && zero.transfers == 0); CHECK(device.calls == 0);
  const auto completed = runToTerminal(driver, device, 3);
  CHECK(completed.done && completed.status.ok()); CHECK(driver.isInitialized());
  CHECK(driver.resultPending() && !driver.operationActive());
  checkExcludedApis(driver, device);
  const unsigned calls = device.calls;
  const auto again = driver.poll(device.ms, 255);
  CHECK(again.done && again.status.ok() && again.transfers == 0); CHECK(device.calls == calls);
  CHECK(driver.cancel().is(t::Err::BUSY));
  CHECK(driver.getOperationSnapshot().status.ok() && driver.getOperationSnapshot().token == token);
  t::OperationResult result; result.token = 9876; result.hasSample = true;
  CHECK(driver.takeResult(token + 1U, result).is(t::Err::TOKEN_MISMATCH));
  CHECK(result.token == 9876 && result.hasSample && driver.resultPending());
  CHECK(driver.takeResult(token, result).ok()); CHECK(result.token == token && result.status.ok());
  CHECK(result.kind == t::OperationKind::INITIALIZE && !result.hasSample);
  CHECK(!driver.resultPending()); CHECK(device.calls == calls);
  result.token = 6789;
  CHECK(driver.takeResult(token, result).is(t::Err::RESULT_NOT_AVAILABLE)); CHECK(result.token == 6789);
  CHECK(driver.poll(device.ms, 1).status.is(t::Err::RESULT_NOT_AVAILABLE));
}

static void budgetsAndHealth() {
  for (const uint8_t budget : {uint8_t{1}, uint8_t{2}, uint8_t{255}}) {
    Device device; t::TMP1x2 driver; t::OperationToken token = 0;
    CHECK(driver.bind(device.config()).ok());
    CHECK(driver.startInitialize(0, 1000, token).inProgress());
    CHECK(runToTerminal(driver, device, budget).status.ok());
    CHECK(driver.totalSuccess() == device.calls && driver.totalFailures() == 0);
    CHECK(device.yields == 0);
    for (unsigned i = 0; i < device.calls; ++i)
      CHECK(device.history[i].timeout > 0 && device.history[i].timeout <= 50);
  }
  Device device; t::TMP1x2 driver; t::OperationToken token = 0;
  CHECK(driver.bind(device.config()).ok());
  CHECK(driver.startInitialize(0, 7, token).inProgress());
  const auto result = driver.poll(0, 255);
  uint32_t grants = 0;
  for (unsigned i = 0; i < device.calls; ++i) grants += device.history[i].timeout;
  CHECK(grants <= 7 && device.calls == result.transfers);
  CHECK(driver.totalFailures() == 0);
}

static void deadlinesAndWrap() {
  for (bool withClock : {false, true}) {
    Device device; t::TMP1x2 driver; t::OperationToken token = 0; device.ms = 100;
    CHECK(driver.bind(device.config(withClock)).ok());
    CHECK(driver.startInitialize(100, 10, token).inProgress());
    device.ms = 110;
    const auto result = driver.poll(withClock ? 0 : device.ms, 0);
    CHECK(result.done && result.status.is(t::Err::OPERATION_TIMEOUT)); CHECK(device.calls == 0);
    CHECK(driver.totalFailures() == 0);
  }
  {
    Device device; t::TMP1x2 driver; t::OperationToken token = 0;
    CHECK(driver.bind(device.config(true)).ok());
    CHECK(driver.startInitialize(0, 5, token).inProgress());
    device.callbackDuration = 5;
    const auto result = driver.poll(0, 255);
    CHECK(result.done && result.status.is(t::Err::OPERATION_TIMEOUT));
    CHECK(device.calls == 1 && device.history[0].timeout <= 5);
    CHECK(driver.totalSuccess() == 1 && driver.totalFailures() == 0);
  }
  {
    Device device; t::TMP1x2 driver; auto config = device.config();
    device.ms = UINT32_MAX - 10U; config.mode = t::Mode::SHUTDOWN;
    CHECK(initialize(driver, device, config)); CHECK(device.ms < 100);
    t::OperationToken token = 0;
    CHECK(driver.startRead(device.ms, 500, token).inProgress());
    CHECK(runToTerminal(driver, device, 2).status.ok());
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    CHECK(result.hasSample && result.sample.celsius == 25.0f);
  }
}

static void externalClockConversions() {
  for (bool extended : {false, true}) {
    Device device; t::TMP1x2 driver; auto config = device.config();
    config.extendedMode = extended; config.mode = t::Mode::SHUTDOWN;
    CHECK(initialize(driver, device, config));
    CHECK(!device.prematureFormat && device.yields == 0 && !driver.hardwareConfigDirty());
    const unsigned before = device.calls;
    t::OperationToken token = 0;
    CHECK(driver.startRead(device.ms, 500, token).inProgress()); CHECK(device.calls == before);
    CHECK(runToTerminal(driver, device, 3).status.ok());
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    CHECK(result.hasSample && result.sample.celsius == 25.0f && result.sample.extendedMode == extended);
    CHECK(device.yields == 0); CHECK(driver.hasSample());
    bool triggered = false;
    uint32_t triggerTime = 0;
    for (unsigned i = before; i < device.calls; ++i) {
      const auto& transfer = device.history[i];
      if (transfer.write && transfer.reg == 1 && (transfer.value & 0x8100U) == 0x8100U) {
        triggered = true; triggerTime = transfer.at;
      }
      if (!transfer.write && transfer.reg == 0) {
        CHECK(triggered); CHECK(static_cast<uint32_t>(transfer.at - triggerTime) >= 35U);
      }
    }
    CHECK(triggered);
  }
}

static void configurationAdmission() {
  Device device; t::TMP1x2 driver; CHECK(initialize(driver, device, device.config()));
  const auto original = driver.getConfig(); const unsigned calls = device.calls;
  auto changed = original; t::OperationToken token = 987;
  changed.i2cAddress = 0x49;
  CHECK(driver.startConfigure(changed, device.ms, 1000, token).is(t::Err::INVALID_PARAM));
  CHECK(token == 987 && device.calls == calls);
  changed = original; changed.i2cUser = nullptr;
  CHECK(driver.startConfigure(changed, device.ms, 1000, token).is(t::Err::INVALID_PARAM));
  changed = original; changed.nowMs = Device::clock;
  CHECK(driver.startConfigure(changed, device.ms, 1000, token).is(t::Err::INVALID_PARAM));
  changed = original; changed.lowThresholdC = 90;
  CHECK(!driver.startConfigure(changed, device.ms, 1000, token).ok()); CHECK(token == 987);
  changed = original; changed.extendedMode = true;
  CHECK(driver.startConfigure(changed, device.ms, 1000, token).inProgress());
  CHECK(!driver.getConfig().extendedMode); CHECK(device.calls == calls);
  CHECK(driver.cancel().is(t::Err::CANCELLED)); CHECK(device.calls == calls);
  CHECK(!driver.getConfig().extendedMode && !driver.hardwareConfigDirty());
  t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
  CHECK(result.status.is(t::Err::CANCELLED) && !result.hardwareEffectPossible);
}

static void initializationFailureMatrix() {
  for (bool extended : {false, true}) {
    Device baseline; t::TMP1x2 good; auto config = baseline.config(); config.extendedMode = extended;
    CHECK(initialize(good, baseline, config)); const unsigned transfers = baseline.calls;
    for (unsigned failed = 1; failed <= transfers; ++failed) {
      for (bool accepted : {false, true}) {
        Device device; t::TMP1x2 driver; auto candidate = device.config(); candidate.extendedMode = extended;
        CHECK(driver.bind(candidate).ok()); device.failAt = failed; device.acceptFailedWrite = accepted;
        t::OperationToken token = 0; CHECK(driver.startInitialize(0, 1000, token).inProgress());
        CHECK(runToTerminal(driver, device, 2).status.is(t::Err::I2C_TIMEOUT));
        CHECK(device.calls == failed && driver.totalFailures() == 1);
        t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
        CHECK(result.status.detail == 731 && !result.hasSample && !driver.hasSample());
        CHECK(!driver.isInitialized()); device.failAt = 0;
        CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
        CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
        CHECK(driver.isInitialized() && !driver.hardwareConfigDirty()); CHECK(!device.prematureFormat);
        CHECK(driver.startRead(device.ms, 500, token).inProgress());
        CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
        CHECK(result.hasSample && result.sample.celsius == 25.0f && result.sample.extendedMode == extended);
      }
    }
  }
}

static void cancellationAndTeardown() {
  Device baseline; t::TMP1x2 good; CHECK(initialize(good, baseline, baseline.config()));
  auto candidate = good.getConfig(); candidate.extendedMode = true;
  t::OperationToken token = 0; const unsigned begin = baseline.calls;
  CHECK(good.startConfigure(candidate, baseline.ms, 1000, token).inProgress());
  CHECK(runToTerminal(good, baseline).status.ok()); const unsigned transfers = baseline.calls - begin;
  for (unsigned stopAfter = 0; stopAfter < transfers; ++stopAfter) {
    Device device; t::TMP1x2 driver; CHECK(initialize(driver, device, device.config()));
    auto next = driver.getConfig(); next.extendedMode = true;
    const unsigned start = device.calls; const unsigned initialWrites = device.writes;
    CHECK(driver.startConfigure(next, device.ms, 1000, token).inProgress());
    for (unsigned step = 0; device.calls - start < stopAfter && step < 1000; ++step) {
      const auto progress = driver.poll(device.ms, 1); CHECK(!progress.done);
      ++device.ms;
    }
    CHECK(device.calls - start == stopAfter);
    const unsigned calls = device.calls;
    CHECK(driver.cancel().is(t::Err::CANCELLED)); CHECK(device.calls == calls);
    CHECK(driver.resultPending() && !driver.operationActive());
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    CHECK(result.status.is(t::Err::CANCELLED) && !result.hasSample);
    CHECK(result.hardwareEffectPossible == (device.writes != initialWrites));
    if (device.writes != initialWrites) CHECK(driver.hardwareConfigDirty());
    CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
    CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
    CHECK(!driver.hardwareConfigDirty() && !device.prematureFormat);
  }
  Device device; t::TMP1x2 driver; CHECK(initialize(driver, device, device.config()));
  auto next = driver.getConfig(); next.extendedMode = true;
  CHECK(driver.startConfigure(next, device.ms, 1000, token).inProgress());
  const auto oldToken = token;
  // Force an actual write before deinitializing, without relying on phase names.
  const unsigned beforeWrites = device.writes;
  for (unsigned step = 0; device.writes == beforeWrites && step < 1000; ++step) { driver.poll(device.ms, 1); ++device.ms; }
  const unsigned calls = device.calls;
  driver.end(); CHECK(device.calls == calls && !driver.isInitialized());
  CHECK(driver.resultPending() && driver.hardwareConfigDirty());
  t::OperationResult result; CHECK(driver.takeResult(oldToken, result).ok());
  CHECK(result.status.is(t::Err::CANCELLED));
  CHECK(driver.bind(device.config()).ok()); CHECK(driver.hardwareConfigDirty());
  CHECK(driver.startRecover(device.ms, 1000, token).inProgress()); CHECK(token != oldToken);
  CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
  driver.unbind(); CHECK(device.calls >= calls && !driver.isBound());
  CHECK(driver.bind(device.config()).ok());
  const auto previous = token; CHECK(driver.startInitialize(device.ms, 1000, token).inProgress()); CHECK(token != previous);
  CHECK(driver.cancel().is(t::Err::CANCELLED));
  CHECK(driver.takeResult(previous, result).is(t::Err::TOKEN_MISMATCH));
}

static void abandonedConversions() {
  for (bool expire : {false, true}) {
    Device device; t::TMP1x2 driver; auto config = device.config(); config.mode = t::Mode::SHUTDOWN;
    CHECK(initialize(driver, device, config));
    const unsigned writes = device.writes; t::OperationToken token = 0;
    CHECK(driver.startRead(device.ms, 10, token).inProgress());
    for (unsigned step = 0; device.writes == writes && step < 20; ++step) {
      CHECK(!driver.poll(device.ms, 1).done); ++device.ms;
    }
    CHECK(device.shotPending); CHECK(device.writes > writes);
    const uint32_t triggered = device.shotAt; const unsigned calls = device.calls;
    if (expire) {
      device.ms += 10;
      CHECK(driver.poll(device.ms, 0).status.is(t::Err::OPERATION_TIMEOUT));
    } else {
      CHECK(driver.cancel().is(t::Err::CANCELLED));
    }
    CHECK(device.calls == calls && !driver.hasSample()); CHECK(driver.hardwareConfigDirty());
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    CHECK(!result.hasSample && result.hardwareEffectPossible);
    CHECK(!driver.startRead(device.ms, 500, token).inProgress()); CHECK(device.calls == calls);
    CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
    CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
    CHECK(!driver.hardwareConfigDirty());
    // Recovery may write SD to settle, but must not start another conversion
    // before the abandoned conversion's conservative interval has elapsed.
    for (unsigned i = calls; i < device.calls; ++i) {
      const auto& transfer = device.history[i];
      if (transfer.write && transfer.reg == 1 && (transfer.value & 0x8100U) == 0x8100U)
        CHECK(static_cast<uint32_t>(transfer.at - triggered) >= 35U);
    }
    CHECK(driver.startRead(device.ms, 500, token).inProgress());
    CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
    CHECK(result.hasSample && result.sample.celsius == 25.0f);
  }
}

static void readFailureAndLatePublication() {
  Device baseline; t::TMP1x2 good; auto config = baseline.config(); config.mode = t::Mode::SHUTDOWN;
  CHECK(initialize(good, baseline, config)); t::OperationToken token = 0;
  const unsigned start = baseline.calls;
  CHECK(good.startRead(baseline.ms, 500, token).inProgress());
  CHECK(runToTerminal(good, baseline).status.ok()); const unsigned transfers = baseline.calls - start;
  for (unsigned fail = 1; fail <= transfers; ++fail) {
    for (bool accepted : {false, true}) {
      Device device; t::TMP1x2 driver; auto candidate = device.config(); candidate.mode = t::Mode::SHUTDOWN;
      CHECK(initialize(driver, device, candidate));
      device.failAt = device.calls + fail; device.acceptFailedWrite = accepted;
      CHECK(driver.startRead(device.ms, 500, token).inProgress());
      CHECK(runToTerminal(driver, device).status.is(t::Err::I2C_TIMEOUT));
      t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
      CHECK(result.status.detail == 731 && !result.hasSample && !driver.hasSample());
      CHECK(driver.totalFailures() == 1); device.failAt = 0;
      CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
      CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
      CHECK(driver.startRead(device.ms, 500, token).inProgress());
      CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
      CHECK(result.hasSample && result.sample.celsius == 25.0f);
    }
  }
  Device device; t::TMP1x2 driver; CHECK(initialize(driver, device, device.config(true)));
  CHECK(!driver.hasSample()); device.callbackDuration = 5;
  CHECK(driver.startRead(device.ms, 5, token).inProgress());
  const auto late = driver.poll(device.ms, 1);
  CHECK(late.done && late.status.is(t::Err::OPERATION_TIMEOUT));
  t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
  CHECK(!result.hasSample && !driver.hasSample()); CHECK(driver.totalFailures() == 0);
}

static void finalCallbackCompletesImmediately() {
  for (const auto mode : {t::Mode::CONTINUOUS, t::Mode::SHUTDOWN}) {
    Device device; t::TMP1x2 driver; auto config = device.config(); config.mode = mode;
    CHECK(initialize(driver, device, config)); t::OperationToken token = 0;
    CHECK(driver.startRead(device.ms, 500, token).inProgress());
    bool observed = false;
    for (unsigned step = 0; step < 1000; ++step) {
      const unsigned before = device.calls;
      const auto progress = driver.poll(device.ms, 1);
      CHECK(device.calls - before <= 1);
      if (device.calls != before && !device.history[before].write && device.history[before].reg == 0) {
        CHECK(progress.done && progress.status.ok() && progress.transfers == 1);
        CHECK(driver.resultPending() && driver.hasSample()); observed = true; break;
      }
      CHECK(!progress.done); ++device.ms;
    }
    CHECK(observed);
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok()); CHECK(result.hasSample);
    auto changed = driver.getConfig(); changed.conversionRate = t::ConversionRate::HZ_1;
    CHECK(driver.startConfigure(changed, device.ms, 500, token).inProgress()); observed = false;
    for (unsigned step = 0; step < 1000; ++step) {
      const unsigned before = device.calls;
      const auto progress = driver.poll(device.ms, 1);
      if (device.calls != before && !device.history[before].write && device.history[before].reg == 3) {
        CHECK(progress.done && progress.status.ok()); CHECK(driver.resultPending()); observed = true; break;
      }
      CHECK(!progress.done); ++device.ms;
    }
    CHECK(observed && !driver.hardwareConfigDirty());
  }
}

static void manualConversionOwnership() {
  for (bool endFirst : {false, true}) {
    Device device; t::TMP1x2 driver; auto config = device.config(); config.mode = t::Mode::SHUTDOWN;
    CHECK(initialize(driver, device, config));
    CHECK(driver.startOneShot().ok()); CHECK(device.shotPending);
    t::OperationToken token = 7654; const unsigned calls = device.calls;
    CHECK(driver.startRead(device.ms, 500, token).is(t::Err::BUSY)); CHECK(token == 7654);
    CHECK(driver.recover().is(t::Err::BUSY)); CHECK(driver.conversionStarted()); CHECK(device.calls == calls);
    if (endFirst) { driver.end(); CHECK(driver.hardwareConfigDirty()); }
    const auto rebound = driver.bind(config);
    CHECK(device.calls == calls);
    if (rebound.is(t::Err::BUSY)) {
      CHECK(driver.conversionStarted()); driver.end(); CHECK(driver.bind(config).ok());
    } else {
      CHECK(rebound.ok()); CHECK(driver.hardwareConfigDirty());
    }
    CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
    CHECK(runToTerminal(driver, device).status.ok());
    t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    CHECK(!driver.hardwareConfigDirty()); CHECK(device.yields == 0);
  }
}

static void configurationFailureMatrix() {
  for (bool initiallyExtended : {false, true}) {
    Device baseline; t::TMP1x2 good; auto config = baseline.config(); config.extendedMode = initiallyExtended;
    CHECK(initialize(good, baseline, config));
    auto desired = good.getConfig(); desired.extendedMode = !initiallyExtended;
    desired.lowThresholdC = 20.03f; desired.highThresholdC = 30.06f;
    t::OperationToken token = 0; const unsigned before = baseline.calls;
    CHECK(good.startConfigure(desired, baseline.ms, 1000, token).inProgress());
    CHECK(runToTerminal(good, baseline).status.ok()); const unsigned transfers = baseline.calls - before;
    CHECK(good.getConfig().lowThresholdC == 20.0f && good.getConfig().highThresholdC == 30.0625f);
    for (unsigned fail = 1; fail <= transfers; ++fail) {
      for (bool accepted : {false, true}) {
        Device device; t::TMP1x2 driver; auto initial = device.config(); initial.extendedMode = initiallyExtended;
        CHECK(initialize(driver, device, initial));
        auto target = driver.getConfig(); target.extendedMode = !initiallyExtended;
        target.lowThresholdC = 20.03f; target.highThresholdC = 30.06f;
        const unsigned start = device.calls; const unsigned writes = device.writes;
        device.failAt = start + fail; device.acceptFailedWrite = accepted;
        CHECK(driver.startConfigure(target, device.ms, 1000, token).inProgress());
        CHECK(runToTerminal(driver, device, 2).status.is(t::Err::I2C_TIMEOUT));
        CHECK(device.calls == start + fail && driver.totalFailures() == 1);
        t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
        CHECK(result.status.detail == 731 && !result.hasSample);
        const bool written = device.writes != writes;
        CHECK(result.hardwareEffectPossible == written);
        CHECK(driver.getConfig().extendedMode == (written ? !initiallyExtended : initiallyExtended));
        if (written) {
          CHECK(driver.hardwareConfigDirty());
          CHECK(driver.getConfig().lowThresholdC == 20.0f && driver.getConfig().highThresholdC == 30.0625f);
        }
        const bool wantedFormat = driver.getConfig().extendedMode;
        device.failAt = 0;
        CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
        CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
        CHECK(!driver.hardwareConfigDirty() && !device.prematureFormat);
        CHECK(driver.startRead(device.ms, 500, token).inProgress());
        CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
        CHECK(result.hasSample && result.sample.celsius == 25.0f && result.sample.extendedMode == wantedFormat);
      }
    }
  }
}

static void protocolFailuresAndStalledOwnerTime() {
  {
    Device baseline; t::TMP1x2 good;
    CHECK(initialize(good, baseline, baseline.config())); const unsigned transfers = baseline.calls;
    Device device; t::TMP1x2 driver; CHECK(driver.bind(device.config()).ok());
    device.corruptAt = transfers; device.corruptMask = 0x0010;
    t::OperationToken token = 0; CHECK(driver.startInitialize(0, 1000, token).inProgress());
    const auto failed = runToTerminal(driver, device);
    CHECK(failed.status.is(t::Err::CONFIG_MISMATCH)); CHECK(driver.hardwareConfigDirty());
    CHECK(driver.totalFailures() == 0 && driver.totalSuccess() == device.calls);
    CHECK(driver.lastError().ok()); t::OperationResult result; CHECK(driver.takeResult(token, result).ok());
    device.corruptAt = 0;
    CHECK(driver.startRecover(device.ms, 1000, token).inProgress());
    CHECK(runToTerminal(driver, device).status.ok()); CHECK(driver.takeResult(token, result).ok());
  }
  {
    Device device; t::TMP1x2 driver; auto config = device.config(); config.mode = t::Mode::SHUTDOWN;
    CHECK(initialize(driver, device, config)); device.neverReady = true;
    t::OperationToken token = 0; CHECK(driver.startRead(device.ms, 100, token).inProgress());
    const auto failed = runToTerminal(driver, device, 255);
    CHECK(failed.status.is(t::Err::OPERATION_TIMEOUT)); CHECK(driver.hardwareConfigDirty());
    CHECK(driver.totalFailures() == 0 && driver.lastError().ok()); CHECK(!driver.hasSample());
  }
  {
    Device device; t::TMP1x2 driver; auto config = device.config(); config.extendedMode = true;
    CHECK(driver.bind(config).ok()); t::OperationToken token = 0;
    CHECK(driver.startInitialize(0, 1000, token).inProgress());
    for (unsigned step = 0; !driver.getOperationSnapshot().waiting && step < 50; ++step) {
      CHECK(!driver.poll(0, 1).done);
    }
    CHECK(driver.getOperationSnapshot().waiting);
    // The first later poll supplies the conservative completion-time anchor.
    CHECK(!driver.poll(0, 1).done); const unsigned calls = device.calls;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
      const auto waiting = driver.poll(0, 255);
      CHECK(!waiting.done && waiting.status.inProgress() && waiting.transfers == 0);
      CHECK(waiting.nextPollMs >= 35);
    }
    CHECK(device.calls == calls && device.yields == 0);
    CHECK(driver.poll(1000, 0).status.is(t::Err::OPERATION_TIMEOUT)); CHECK(device.calls == calls);
  }
}

static void schedulingHintsAndReleasedSnapshots() {
  for (const uint32_t started : {uint32_t{100}, UINT32_MAX - 5U}) {
    Device device; t::TMP1x2 driver; device.ms = started;
    auto config = device.config(); config.mode = t::Mode::SHUTDOWN;
    CHECK(driver.bind(config).ok()); t::OperationToken token = 0;
    CHECK(driver.startInitialize(started, 10, token).inProgress());
    for (unsigned step = 0; !driver.getOperationSnapshot().waiting && step < 50; ++step)
      CHECK(!driver.poll(started, 1).done);
    CHECK(driver.getOperationSnapshot().waiting);
    const auto anchor = driver.poll(started, 1);
    CHECK(!anchor.done && anchor.transfers == 0);
    CHECK(anchor.nextPollMs == static_cast<uint32_t>(started + 10U));
    CHECK(driver.getOperationSnapshot().nextPollMs == anchor.nextPollMs);
    const unsigned calls = device.calls;
    driver.unbind(); CHECK(device.calls == calls);
    const auto cleared = driver.getOperationSnapshot();
    CHECK(!cleared.active && !cleared.resultPending && cleared.token == 0);
    CHECK(cleared.kind == t::OperationKind::NONE && cleared.status.ok());
    CHECK(cleared.startedMs == 0 && cleared.timeoutMs == 0 && !cleared.waiting && cleared.nextPollMs == 0);
    CHECK(!cleared.hardwareEffectPossible && cleared.desiredConfig.i2cWrite == nullptr);
    CHECK(driver.bind(config).ok()); const auto previous = token;
    CHECK(driver.startInitialize(device.ms, 1000, token).inProgress()); CHECK(token != previous);
  }
}

int main() {
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    {"owner admission and retained results", admissionAndResults},
    {"owner transfer budgets and health", budgetsAndHealth},
    {"owner deadlines and wrap", deadlinesAndWrap},
    {"owner external-clock conversions", externalClockConversions},
    {"owner configuration admission", configurationAdmission},
    {"owner initialization failure matrix", initializationFailureMatrix},
    {"owner cancellation and teardown", cancellationAndTeardown},
    {"owner abandoned conversion evidence", abandonedConversions},
    {"owner read failure and late publication", readFailureAndLatePublication},
    {"owner final callback completion", finalCallbackCompletesImmediately},
    {"owner manual-conversion ownership", manualConversionOwnership},
    {"owner configuration failure matrix", configurationFailureMatrix},
    {"owner protocol failures and stalled time", protocolFailuresAndStalledOwnerTime},
    {"owner scheduling hints and released snapshots", schedulingHintsAndReleasedSnapshots},
  };
  for (const auto& test : tests) {
    const int before = failures; test.run();
    if (failures == before) std::printf("[PASS] %s\n", test.name);
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
