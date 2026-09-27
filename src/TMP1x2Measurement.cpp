/// @file TMP1x2Measurement.cpp
/// @brief Temperature encoding, manual conversions, and cached samples.
#include "TMP1x2/TMP1x2.h"
#include <cmath>

namespace TMP1x2 {
namespace {
Status notReady() {
  return Status::Error(Err::MEASUREMENT_NOT_READY, "No completed measurement available");
}
int16_t signedCounts(uint16_t raw, bool extended) {
  const int32_t code = static_cast<int32_t>(raw >> (extended ? 3 : 4));
  const int32_t sign = extended ? 4096 : 2048;
  return static_cast<int16_t>((code & sign) ? code - 2 * sign : code);
}
} // namespace

void TMP1x2::clearConversion() {
  _conversionStarted = _conversionReady = _conversionClockKnown = false;
  _conversionStartMs = 0;
}

Status TMP1x2::readSample(Sample& out) {
  Status status = guard(true);
  if (!status.ok()) return status;
  uint16_t raw = 0;
  status = read(cmd::REG_TEMPERATURE, raw, true);
  if (!status.ok()) return status;
  Sample sample;
  status = decodeObservedTemperature(raw, sample);
  if (!status.ok()) return status;
  sample.timestampMs = now();
  sample.timestampValid = clockKnown();
  _lastSample = sample;
  _hasSample = true;
  out = sample;
  return Status::Ok();
}

Status TMP1x2::decodeObservedTemperature(uint16_t raw, Sample& sample) {
  const bool formatMismatch = ((raw & cmd::MASK_TEMP_EXTENDED) != 0) != _config.extendedMode;
  if (formatMismatch) _formatRefreshPending = true;
  Status status = decodeTemperature(raw, sample);
  if (!status.ok()) { markDirty(status); return status; }
  if (formatMismatch) {
    markDirty(Status::Error(Err::CONFIG_MISMATCH, "Temperature format differs from desired settings", raw));
    return notReady();
  }
  return Status::Ok();
}

Status TMP1x2::readTemperature(float& out) {
  Sample sample;
  const Status status = readSample(sample);
  if (status.ok()) out = sample.celsius;
  return status;
}

Status TMP1x2::readTemperatureFahrenheit(float& out) {
  float celsius = 0;
  const Status status = readTemperature(celsius);
  if (status.ok()) out = celsiusToFahrenheit(celsius);
  return status;
}

Status TMP1x2::readTemperatureCounts(int16_t& out) {
  Sample sample;
  const Status status = readSample(sample);
  if (status.ok()) out = sample.counts;
  return status;
}

Status TMP1x2::startOneShot() {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (_conversionStarted) return Status::Error(Err::BUSY, "One-shot conversion pending");
  if (_config.mode != Mode::SHUTDOWN)
    return Status::Error(Err::INVALID_CONFIG, "One-shot requires SHUTDOWN mode");
  uint16_t current = 0;
  status = read(cmd::REG_CONFIG, current, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(current).valid ||
      (current & cmd::MASK_WRITABLE_CONFIG) !=
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", current);
    markConfigurationDirty(status, current); return status;
  }
  if (!(current & cmd::MASK_OS))
    return Status::Error(Err::BUSY, "Previous shutdown conversion is still active");
  status = write(cmd::REG_CONFIG, packConfiguration(_config) | cmd::MASK_OS, true);
  if (!status.ok()) { markDirty(status); return status; }
  _conversionStarted = true;
  _conversionReady = false;
  _conversionClockKnown = clockKnown();
  _conversionStartMs = now();
  return Status::Ok();
}

Status TMP1x2::isConversionReady(bool& ready) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (!_conversionStarted) { ready = false; return Status::Ok(); }
  if (_conversionReady) { ready = true; return Status::Ok(); }
  if (_conversionClockKnown && static_cast<uint32_t>(now() - _conversionStartMs) <
      cmd::CONVERSION_TIME_MAX_MS) { ready = false; return Status::Ok(); }
  uint16_t raw = 0;
  status = read(cmd::REG_CONFIG, raw, true);
  if (!status.ok()) return status;
  if (!decodeConfiguration(raw).valid ||
      (raw & cmd::MASK_WRITABLE_CONFIG) !=
          (packConfiguration(_config) & cmd::MASK_WRITABLE_CONFIG)) {
    status = Status::Error(Err::CONFIG_MISMATCH, "One-shot configuration changed", raw);
    markConfigurationDirty(status, raw); return status;
  }
  _conversionReady = (raw & cmd::MASK_OS) != 0;
  ready = _conversionReady;
  return Status::Ok();
}

Status TMP1x2::tryRead(Sample& out) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (_config.mode == Mode::CONTINUOUS) return readSample(out);
  if (!_conversionStarted) return notReady();
  bool ready = false;
  status = isConversionReady(ready);
  if (!status.ok()) return status;
  if (!ready) return notReady();
  status = readSample(out);
  if (status.ok()) {
    out.freshConversion = true;
    _lastSample.freshConversion = true;
    clearConversion();
  }
  return status;
}

Status TMP1x2::readBlocking(Sample& out, uint32_t timeoutMs) {
  Status status = guard(true);
  if (!status.ok()) return status;
  if (!_config.nowMs)
    return Status::Error(Err::INVALID_CONFIG, "Blocking read requires nowMs callback");
  if (timeoutMs == 0 || timeoutMs > 0x7FFFFFFFu)
    return Status::Error(Err::INVALID_PARAM, "Timeout must be 1..INT32_MAX ms");
  if (_config.mode == Mode::CONTINUOUS) return readSample(out);
  const uint32_t started = now();
  if (!_conversionStarted) {
    status = startOneShot();
    if (!status.ok()) return status;
  }
  uint32_t previous = started;
  uint32_t unchanged = 0;
  for (;;) {
    const uint32_t current = now();
    if (static_cast<uint32_t>(current - started) >= timeoutMs)
      return Status::Error(Err::TIMEOUT, "One-shot polling deadline expired");
    status = tryRead(out);
    if (!status.is(Err::MEASUREMENT_NOT_READY)) return status;
    if (current == previous) {
      if (++unchanged >= 1000000u)
        return Status::Error(Err::INVALID_CONFIG, "nowMs clock did not advance");
    } else { previous = current; unchanged = 0; }
    if (_config.cooperativeYield) _config.cooperativeYield(_config.timeUser);
  }
}

Status TMP1x2::decodeTemperature(uint16_t raw, Sample& out) {
  const bool extended = (raw & cmd::MASK_TEMP_EXTENDED) != 0;
  if ((raw & (extended ? 0x0006u : 0x000Eu)) != 0)
    return Status::Error(Err::CONFIG_MISMATCH, "Temperature reserved bits are nonzero", raw);
  Sample sample;
  sample.raw = raw;
  sample.extendedMode = extended;
  sample.counts = signedCounts(raw, extended);
  sample.celsius = static_cast<float>(sample.counts) * 0.0625f;
  out = sample;
  return Status::Ok();
}

Status TMP1x2::encodeThreshold(float celsius, bool extended, uint16_t& out) {
  const float minimum = minimumThresholdC(extended);
  const float maximum = maximumThresholdC(extended);
  if (!std::isfinite(celsius) || celsius < minimum || celsius > maximum)
    return Status::Error(Err::INVALID_PARAM, "Threshold outside register range");
  const int32_t counts = static_cast<int32_t>(std::round(celsius * 16.0f));
  const uint16_t bits = static_cast<uint16_t>(counts) & (extended ? 0x1FFFu : 0x0FFFu);
  out = static_cast<uint16_t>(bits << (extended ? 3 : 4));
  return Status::Ok();
}

Status TMP1x2::celsiusToCounts(float celsius, bool extended, int16_t& out) {
  uint16_t raw = 0;
  const Status status = encodeThreshold(celsius, extended, raw);
  if (status.ok()) out = signedCounts(raw, extended);
  return status;
}

float TMP1x2::decodeThreshold(uint16_t raw, bool extended) {
  return static_cast<float>(signedCounts(raw, extended)) * 0.0625f;
}

Status TMP1x2::getLastSample(Sample& out) const {
  if (!_hasSample) return notReady();
  out = _lastSample;
  return Status::Ok();
}

Status TMP1x2::getLastSample(Sample& out, uint32_t nowMs, uint32_t maxAgeMs) const {
  if (maxAgeMs > 0x7FFFFFFFU)
    return Status::Error(Err::INVALID_PARAM, "Sample age budget must be 0..INT32_MAX ms");
  if (!_hasSample) return notReady();
  if (_dirty) return Status::Error(Err::INVALID_CONFIG, "Cached sample configuration is no longer trusted");
  if (!sampleFresh(nowMs, maxAgeMs)) return notReady();
  out = _lastSample;
  return Status::Ok();
}

bool TMP1x2::sampleFresh(uint32_t nowMs, uint32_t maxAgeMs) const {
  return _initialized && !_dirty && _hasSample && _lastSample.timestampValid &&
      maxAgeMs <= 0x7FFFFFFFU && static_cast<uint32_t>(nowMs - _lastSample.timestampMs) <= maxAgeMs;
}

uint32_t TMP1x2::sampleAgeMs() const {
  return _hasSample ? static_cast<uint32_t>(now() - _lastSample.timestampMs) : UINT32_MAX;
}

uint32_t TMP1x2::sampleAgeMs(uint32_t nowMs) const {
  return _hasSample ? static_cast<uint32_t>(nowMs - _lastSample.timestampMs) : UINT32_MAX;
}
} // namespace TMP1x2
