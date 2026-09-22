// Native ESP-IDF example. One application task owns the bus and driver. The
// input task queues characters only; it never touches I2C or driver state.
#include <cstdarg>
#include <cstdio>
#include <climits>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include "BoardConfig.h"
#include "Tmp1x2Cli.h"

namespace {
struct App {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t devices[4]{};
  tmp1x2_cli::TransferStats stats{};
  QueueHandle_t input = nullptr;
  tmp1x2_cli::Cli cli{};
} app;
TMP1x2::Status mapError(esp_err_t error) {
  if (error == ESP_OK) return TMP1x2::Status::Ok();
  // Transaction errors do not reliably identify which NACK phase occurred.
  return TMP1x2::Status::Error(error == ESP_ERR_TIMEOUT ? TMP1x2::Err::I2C_TIMEOUT : TMP1x2::Err::I2C_ERROR,
                               "ESP-IDF I2C transfer", error);
}
TMP1x2::Status finish(esp_err_t error) { app.stats.record(error == ESP_OK); return mapError(error); }
TMP1x2::Status writeI2c(uint8_t address, const uint8_t* data, size_t length,
                       uint32_t timeoutMs, void*) {
  if (address < 0x48 || address > 0x4B || !data || length == 0 || timeoutMs == 0 || timeoutMs > INT_MAX)
    return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "invalid IDF write");
  return finish(i2c_master_transmit(app.devices[address - 0x48], data, length, static_cast<int>(timeoutMs)));
}
TMP1x2::Status readI2c(uint8_t address, const uint8_t* tx, size_t txLength,
                      uint8_t* rx, size_t rxLength, uint32_t timeoutMs, void*) {
  if (address < 0x48 || address > 0x4B || !tx || txLength == 0 || !rx || rxLength == 0 || timeoutMs == 0 || timeoutMs > INT_MAX)
    return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "invalid IDF read");
  return finish(i2c_master_transmit_receive(app.devices[address - 0x48], tx, txLength, rx, rxLength, static_cast<int>(timeoutMs)));
}
TMP1x2::Status probe(uint8_t address, void*) {
  const esp_err_t result = i2c_master_probe(app.bus, address, static_cast<int>(board::I2C_TIMEOUT_MS));
  app.stats.record(result == ESP_OK);
  // Probe is an address-only operation; ESP_ERR_NOT_FOUND proves address NACK.
  if (result == ESP_ERR_NOT_FOUND)
    return TMP1x2::Status::Error(TMP1x2::Err::I2C_NACK_ADDR, "address probe NACK", result);
  return mapError(result);
}
uint32_t nowMs(void*) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void cooperativeYield(void*) { vTaskDelay(1); }
void output(void*, const char* format, va_list args) { std::vprintf(format, args); }
tmp1x2_cli::TransferStats stats(void*) { return app.stats; }
void inputTask(void*) {
  while (true) {
    const int value = std::getchar();
    if (value == EOF) { std::clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    const char character = static_cast<char>(value);
    // A bounded queue provides backpressure; never silently drop part of a
    // command (which could turn an invalid line into a valid write).
    (void)xQueueSend(app.input, &character, portMAX_DELAY);
  }
}
}  // namespace

extern "C" void app_main() {
  i2c_master_bus_config_t bus{};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = static_cast<gpio_num_t>(board::I2C_SDA);
  bus.scl_io_num = static_cast<gpio_num_t>(board::I2C_SCL);
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  esp_err_t error = i2c_new_master_bus(&bus, &app.bus);
  if (error != ESP_OK) { std::printf("[E] Bus creation failed: %s\n", esp_err_to_name(error)); return; }
  // Fixed set of four device handles makes runtime address changes allocation
  // free. i2c_master_bus_add_device does not prove presence or touch the chip.
  for (unsigned index = 0; index < 4; ++index) {
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = static_cast<uint16_t>(0x48U + index);
    device.scl_speed_hz = board::I2C_FREQUENCY_HZ;
    error = i2c_master_bus_add_device(app.bus, &device, &app.devices[index]);
    if (error != ESP_OK) {
      std::printf("[E] Device handle creation failed: %s\n", esp_err_to_name(error));
      for (unsigned previous = 0; previous < index; ++previous) (void)i2c_master_bus_rm_device(app.devices[previous]);
      (void)i2c_del_master_bus(app.bus);
      return;
    }
  }
  app.input = xQueueCreate(192, sizeof(char));
  if (!app.input || xTaskCreate(inputTask, "tmp1x2_input", 3072, nullptr, 4, nullptr) != pdPASS) {
    std::puts("[E] Input queue/task creation failed");
    if (app.input) vQueueDelete(app.input);
    for (auto device : app.devices) (void)i2c_master_bus_rm_device(device);
    (void)i2c_del_master_bus(app.bus);
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
  platform.framework = "native-esp-idf";
  platform.frameworkVersion = esp_get_idf_version();
  platform.target = CONFIG_IDF_TARGET;
  app.cli.setup(platform, config);
  while (true) {
    char value = 0;
    for (unsigned count = 0; count < 64 && xQueueReceive(app.input, &value, 0) == pdTRUE; ++count) app.cli.feed(value);
    app.cli.tick();
    vTaskDelay(1);
  }
}
