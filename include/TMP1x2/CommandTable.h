/// @file CommandTable.h
/// @brief TI TMP102/TMP112 register map (MSB first on the wire).
#pragma once
#include <cstdint>
namespace TMP1x2 { namespace cmd {
static constexpr uint8_t I2C_ADDR_DEFAULT = 0x48;
static constexpr uint8_t I2C_ADDR_MIN = 0x48;
static constexpr uint8_t I2C_ADDR_MAX = 0x4B;
static constexpr uint8_t REG_TEMPERATURE = 0x00;
static constexpr uint8_t REG_CONFIG = 0x01;
static constexpr uint8_t REG_TLOW = 0x02;
static constexpr uint8_t REG_THIGH = 0x03;
static constexpr uint8_t REG_LO_THRESH = REG_TLOW;
static constexpr uint8_t REG_HI_THRESH = REG_THIGH;
static constexpr uint16_t CONFIG_DEFAULT = 0x60A0;
static constexpr uint16_t TLOW_DEFAULT = 0x4B00;
static constexpr uint16_t THIGH_DEFAULT = 0x5000;
static constexpr uint16_t MASK_OS = 0x8000;
static constexpr uint16_t MASK_RESOLUTION = 0x6000;
static constexpr uint16_t MASK_FAULT_QUEUE = 0x1800;
static constexpr uint16_t MASK_POLARITY = 0x0400;
static constexpr uint16_t MASK_ALERT_MODE = 0x0200;
static constexpr uint16_t MASK_SHUTDOWN = 0x0100;
static constexpr uint16_t MASK_CONVERSION_RATE = 0x00C0;
static constexpr uint16_t MASK_ALERT = 0x0020;
static constexpr uint16_t MASK_EXTENDED_MODE = 0x0010;
static constexpr uint16_t MASK_WRITABLE_CONFIG = 0x1FD0; // OS excluded: command, not state.
static constexpr uint16_t MASK_RESERVED = 0x000F;
static constexpr uint16_t MASK_TEMP_EXTENDED = 0x0001;
/// Conservative bound covering older 35-ms silicon and newer 15-ms devices.
static constexpr uint32_t CONVERSION_TIME_MAX_MS = 35;
/// General-call reset is deliberately NOT exposed by the driver: it resets
/// every compatible device on the application-owned bus.
static constexpr uint8_t GENERAL_CALL_ADDRESS = 0x00;
static constexpr uint8_t GENERAL_CALL_RESET = 0x06;
static constexpr uint8_t SMBUS_ALERT_RESPONSE_ADDRESS = 0x0C;
} } // namespace TMP1x2::cmd
