#pragma once

// Example defaults only. Change these to the pins and pull-ups on your board.
#ifndef TMP1X2_I2C_SDA
#define TMP1X2_I2C_SDA 8
#endif
#ifndef TMP1X2_I2C_SCL
#define TMP1X2_I2C_SCL 9
#endif
// Opt-in only: connect ALERT to an application-selected input with a pull-up.
#ifndef TMP1X2_ALERT_PIN
#define TMP1X2_ALERT_PIN -1
#endif
namespace board {
inline constexpr int I2C_SDA = TMP1X2_I2C_SDA;
inline constexpr int I2C_SCL = TMP1X2_I2C_SCL;
inline constexpr int ALERT_PIN = TMP1X2_ALERT_PIN;
static_assert(ALERT_PIN >= -1, "TMP1X2_ALERT_PIN must be -1 or a valid input GPIO");
static_assert(ALERT_PIN < 0 || (ALERT_PIN != I2C_SDA && ALERT_PIN != I2C_SCL),
              "TMP1X2_ALERT_PIN must not reuse an I2C bus pin");
inline constexpr unsigned I2C_FREQUENCY_HZ = 400000;
inline constexpr unsigned I2C_TIMEOUT_MS = 50;
inline constexpr unsigned SERIAL_BAUD = 115200;
}  // namespace board
