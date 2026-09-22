// Diagnostic example. This task owns Wire, driver calls, bus pins and policy.
// Production applications must serialize all shared-bus access externally.
#include <Arduino.h>
#include <Wire.h>
#include <cstdarg>
#include <cstdio>
#if defined(ESP32)
#include <esp_system.h>
#include <esp_arduino_version.h>
#endif
#include "../common/BoardConfig.h"
#include "../common/Tmp1x2Cli.h"

namespace {
tmp1x2_cli::Cli cli;
tmp1x2_cli::TransferStats transfers;
TMP1x2::Status mapWire(uint8_t code) {
  using TMP1x2::Err;
  if (code == 0) return TMP1x2::Status::Ok();
  const Err error = code == 2 ? Err::I2C_NACK_ADDR : code == 3 ? Err::I2C_NACK_DATA :
                    code == 5 ? Err::I2C_TIMEOUT : Err::I2C_ERROR;
  return TMP1x2::Status::Error(error, "Wire transfer", code);
}
TMP1x2::Status finish(TMP1x2::Status status) { transfers.record(status.ok()); return status; }
TMP1x2::Status writeI2c(uint8_t address, const uint8_t* data, size_t length,
                       uint32_t timeoutMs, void*) {
  if (!data || !length || timeoutMs == 0 || timeoutMs > UINT16_MAX)
    return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "invalid Wire write");
  Wire.setTimeOut(static_cast<uint16_t>(timeoutMs));
  Wire.beginTransmission(address);
  if (Wire.write(data, length) != length) {
    (void)Wire.endTransmission(true);
    return finish(TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "Wire TX buffer short"));
  }
  return finish(mapWire(Wire.endTransmission(true)));
}
TMP1x2::Status readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                      uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  if (!tx || !txLength || !rx || !rxLength || timeoutMs == 0 || timeoutMs > UINT16_MAX)
    return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "invalid Wire read");
  // ESP32 Wire defers endTransmission(false) until requestFrom; the combined
  // operation below is one repeated-start transaction under the bus timeout.
  Wire.setTimeOut(static_cast<uint16_t>(timeoutMs));
  Wire.beginTransmission(address);
  if (Wire.write(tx, txLength) != txLength) {
    (void)Wire.endTransmission(true);
    return finish(TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "Wire TX buffer short"));
  }
  auto status = mapWire(Wire.endTransmission(false));
  if (!status.ok()) return finish(status);
  const size_t received = Wire.requestFrom(address, rxLength, true);
  if (received != rxLength) {
    while (Wire.available()) (void)Wire.read();
    // requestFrom exposes a length, not a proven address/data NACK phase.
    return finish(TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "Wire short read", static_cast<int32_t>(received)));
  }
  for (size_t index = 0; index < rxLength; ++index) rx[index] = static_cast<uint8_t>(Wire.read());
  return finish(TMP1x2::Status::Ok());
}
TMP1x2::Status probe(uint8_t address, void*) {
  Wire.setTimeOut(static_cast<uint16_t>(board::I2C_TIMEOUT_MS));
  Wire.beginTransmission(address);
  return finish(mapWire(Wire.endTransmission(true)));
}
uint32_t nowMs(void*) { return millis(); }
void cooperativeYield(void*) { delay(1); }
void output(void*, const char* format, va_list args) {
  char buffer[512];
  const int length = vsnprintf(buffer, sizeof(buffer), format, args);
  if (length > 0) Serial.write(reinterpret_cast<const uint8_t*>(buffer),
      static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1);
}
tmp1x2_cli::TransferStats stats(void*) { return transfers; }
}  // namespace

void setup() {
  Serial.begin(board::SERIAL_BAUD);
  const uint32_t started = millis();
  while (!Serial && millis() - started < 3000U) delay(10);
  if (!Wire.begin(board::I2C_SDA, board::I2C_SCL, board::I2C_FREQUENCY_HZ)) {
    Serial.println("[E] Application failed to initialize I2C");
    return;
  }
  TMP1x2::Config config{};
  config.i2cWrite = writeI2c;
  config.i2cWriteRead = readI2c;
  config.nowMs = nowMs;
  config.cooperativeYield = cooperativeYield;
  config.i2cTimeoutMs = board::I2C_TIMEOUT_MS;
  tmp1x2_cli::Platform platform{};
  platform.vprintf = output;
  platform.nowMs = nowMs;
  platform.probeAddress = probe;
  platform.transferStats = stats;
  platform.framework = "Arduino-ESP32";
#ifdef ESP_ARDUINO_VERSION_STR
  platform.frameworkVersion = ESP_ARDUINO_VERSION_STR;
#endif
#ifdef CONFIG_IDF_TARGET
  platform.target = CONFIG_IDF_TARGET;
#endif
  cli.setup(platform, config);
}

void loop() {
  // Fixed per-tick input budget prevents a long host burst starving conversion.
  for (unsigned count = 0; count < 64 && Serial.available(); ++count)
    cli.feed(static_cast<char>(Serial.read()));
  cli.tick();
  delay(1);
}
