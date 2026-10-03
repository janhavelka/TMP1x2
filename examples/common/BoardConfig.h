#pragma once
#include <TMP1x2/Config.h>

// Example defaults only. Change these to the pins and pull-ups on your board.
// Select the BOM and strap before startup performs any device operation.
// 0 = TMP102; 1 = TMP112 classic/fixed ALERT; 2 = TMP112D ADD0 without ALERT.
#ifndef TMP1X2_MODEL
#define TMP1X2_MODEL 0
#endif
#ifndef TMP1X2_I2C_ADDRESS
#if TMP1X2_MODEL == 2
#define TMP1X2_I2C_ADDRESS 0x40
#else
#define TMP1X2_I2C_ADDRESS 0x48
#endif
#endif
#ifndef TMP1X2_I2C_SDA
#define TMP1X2_I2C_SDA 8
#endif
#ifndef TMP1X2_I2C_SCL
#define TMP1X2_I2C_SCL 9
#endif
#ifndef TMP1X2_I2C_FREQUENCY_HZ
#define TMP1X2_I2C_FREQUENCY_HZ 400000
#endif
#ifndef TMP1X2_I2C_TIMEOUT_MS
#define TMP1X2_I2C_TIMEOUT_MS 50
#endif
#ifndef TMP1X2_SERIAL_BAUD
#define TMP1X2_SERIAL_BAUD 115200
#endif
// Opt-in only: connect ALERT to an application-selected input with a pull-up.
#ifndef TMP1X2_ALERT_PIN
#define TMP1X2_ALERT_PIN -1
#endif
namespace board {
static_assert(TMP1X2_MODEL >= 0 && TMP1X2_MODEL <= 2, "TMP1X2_MODEL must be 0, 1 or 2");
inline constexpr TMP1x2::Model SENSOR_MODEL = static_cast<TMP1x2::Model>(TMP1X2_MODEL);
static_assert(TMP1X2_I2C_ADDRESS >= 0 && TMP1X2_I2C_ADDRESS <= 0x7F,
              "TMP1X2_I2C_ADDRESS must be a seven-bit address");
inline constexpr uint8_t I2C_ADDRESS = static_cast<uint8_t>(TMP1X2_I2C_ADDRESS);
static_assert(TMP1x2::isValidAddress(SENSOR_MODEL, I2C_ADDRESS), "Address does not match TMP1X2_MODEL");
inline constexpr int I2C_SDA = TMP1X2_I2C_SDA;
inline constexpr int I2C_SCL = TMP1X2_I2C_SCL;
inline constexpr int ALERT_PIN = TMP1X2_ALERT_PIN;
static_assert(ALERT_PIN >= -1, "TMP1X2_ALERT_PIN must be -1 or a valid input GPIO");
static_assert(ALERT_PIN < 0 || TMP1x2::hasAlertOutput(SENSOR_MODEL),
              "TMP112D address-select package has no ALERT output");
static_assert(ALERT_PIN < 0 || (ALERT_PIN != I2C_SDA && ALERT_PIN != I2C_SCL),
              "TMP1X2_ALERT_PIN must not reuse an I2C bus pin");
// These adapters issue ordinary I2C transactions, without the I2C high-speed
// master code. Faster modes require an application transport that provides it.
static_assert(TMP1X2_I2C_FREQUENCY_HZ > 0 && TMP1X2_I2C_FREQUENCY_HZ <= 400000,
              "Examples support standard/fast-mode I2C up to 400 kHz");
static_assert(TMP1X2_I2C_TIMEOUT_MS > 0 && TMP1X2_I2C_TIMEOUT_MS <= UINT16_MAX,
              "Example timeout must fit the Arduino-ESP32 Wire timeout API");
static_assert(TMP1X2_SERIAL_BAUD > 0, "TMP1X2_SERIAL_BAUD must be positive");
inline constexpr unsigned I2C_FREQUENCY_HZ = TMP1X2_I2C_FREQUENCY_HZ;
inline constexpr unsigned I2C_TIMEOUT_MS = TMP1X2_I2C_TIMEOUT_MS;
inline constexpr unsigned SERIAL_BAUD = TMP1X2_SERIAL_BAUD;
}  // namespace board
