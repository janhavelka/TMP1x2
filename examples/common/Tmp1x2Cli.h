#pragma once

// Example-only framework-neutral command processor. The application owns I2C.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include "TMP1x2/TMP1x2.h"

namespace tmp1x2_cli {
struct TransferStats {
  uint32_t attempts = 0;
  uint32_t successes = 0;
  uint32_t failures = 0;
  void record(bool ok) {
    if (attempts != UINT32_MAX) ++attempts;
    auto& count = ok ? successes : failures;
    if (count != UINT32_MAX) ++count;
  }
};
struct Platform {
  void (*vprintf)(void*, const char*, va_list) = nullptr;
  uint32_t (*nowMs)(void*) = nullptr;
  TMP1x2::Status (*probeAddress)(uint8_t, void*) = nullptr;
  TransferStats (*transferStats)(void*) = nullptr;
  void* user = nullptr;
  const char* framework = "unknown";
  const char* frameworkVersion = "unknown";
  const char* target = "unknown";
};

class Cli {
 public:
  void setup(const Platform& platform, const TMP1x2::Config& config);
  void feed(char value);
  void processCommand(const char* text);
  void tick();
  void printHelp();
  void printPrompt();
 private:
  void print(const char* format, ...);
  void status(TMP1x2::Status value);
  void printVersion();
  void printHealth();
  void printSettings();
  void printSample(const TMP1x2::Sample& sample);
  void stop();
  const char* color(unsigned code) const;
  uint32_t now() const;
  Platform _platform{};
  TMP1x2::Config _config{};
  TMP1x2::TMP1x2 _device{};
  char _line[160]{};
  size_t _length = 0;
  bool _overflow = false;
  bool _color = true;
  bool _watch = false;
  bool _oneShot = false;
  uint32_t _remaining = 0;
  uint32_t _intervalMs = 1000;
  uint32_t _nextMs = 0;
  uint32_t _conversionDeadlineMs = 0;
  uint32_t _watchSuccess = 0;
  uint32_t _watchFailures = 0;
  TMP1x2::Sample _lastSample{};
  bool _hasSample = false;
};
}  // namespace tmp1x2_cli
