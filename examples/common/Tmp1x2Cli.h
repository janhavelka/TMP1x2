#pragma once

// Example-only framework-neutral command processor. The application owns I2C.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include "TMP1x2/TMP1x2.h"
#include "TMP1x2/BusOperations.h"

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
  void (*resetTransferStats)(void*) = nullptr;
  TMP1x2::I2cWriteFn busWrite = nullptr;
  TMP1x2::BusOperations::ReceiveFn busReceive = nullptr;
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
  void printTiming();
  void printTransferStats();
  void printRunStats();
  void recordRunResult(TMP1x2::Status result, const TMP1x2::Sample& sample, bool hasSample = true);
  void tickMixed(uint32_t nowMs);
  TMP1x2::Status startOperation(TMP1x2::OperationKind kind, const TMP1x2::Config* desired = nullptr);
  void finishOperation();
  void printOperation();
  void printOperationResult();
  enum class DiagnosticKind : uint8_t { NONE, SCAN, DISCOVER, SELFCHECK, SETTINGS, SNAPSHOT, CONFIGTEST };
  void startDiagnostic(DiagnosticKind kind);
  void tickDiagnostic();
  void finishDiagnostic(bool cancelled = false);
  void printDiagnostic(bool last = false);
  void printDiagnosticResult();
  void diagnosticCheck(const char* label, TMP1x2::Status result, const char* skip = nullptr);
  void cancelWork();
  void restoreProfile();
  void finishConfigTestOperation(const TMP1x2::OperationResult& result);
  bool activeWork() const;
  void printSample(const TMP1x2::Sample& sample);
  void stop();
  const char* color(unsigned code) const;
  uint32_t now() const;
  Platform _platform{};
  TMP1x2::Config _config{};
  TMP1x2::TMP1x2 _device{};
  int _configuredAlertPin = -1;
  TMP1x2::GpioReadFn _configuredGpioRead = nullptr;
  void* _configuredGpioUser = nullptr;
  TMP1x2::OperationToken _operationToken = 0;
  TMP1x2::OperationResult _lastOperation{};
  bool _hasOperationResult = false;
  bool _lastResultDiagnostic = false;
  struct Diagnostic {
    DiagnosticKind kind = DiagnosticKind::NONE;
    bool active = false;
    bool available = false;
    bool cancelled = false;
    bool restoring = false;
    bool restoreComplete = false;
    uint8_t phase = 0;
    uint8_t nextAddress = 0;
    uint32_t checked = 0;
    uint32_t found = 0;
    uint32_t failures = 0;
    uint32_t passed = 0;
    uint32_t skipped = 0;
    uint32_t startedMs = 0;
    uint32_t endedMs = 0;
    TMP1x2::Status lastError{};
    TMP1x2::Status restoreStatus{};
    TMP1x2::Config baseline{};
  } _diagnostic{}, _lastDiagnostic{};
  char _line[160]{};
  size_t _length = 0;
  bool _overflow = false;
  bool _invalidInput = false;
  bool _color = true;
  bool _verbose = true;
  bool _watch = false;
  bool _oneShot = false;
  uint32_t _remaining = 0;
  uint32_t _intervalMs = 1000;
  uint32_t _nextMs = 0;
  uint32_t _conversionDeadlineMs = 0;
  uint8_t _mixedPhase = 0;
  struct RunStats {
    bool available = false;
    bool hasSample = false;
    bool busAvailable = false;
    bool mixed = false;
    uint32_t target = 0;
    uint32_t successes = 0;
    uint32_t failures = 0;
    uint32_t samples = 0;
    uint32_t completedCycles = 0;
    uint32_t startedMs = 0;
    uint32_t endedMs = 0;
    uint32_t healthSuccessBefore = 0;
    uint32_t healthFailureBefore = 0;
    uint32_t healthSuccessAfter = 0;
    uint32_t healthFailureAfter = 0;
    float minC = 0;
    float maxC = 0;
    double sumC = 0;
    TMP1x2::Status firstError{};
    TMP1x2::Status lastError{};
    TransferStats busBefore{};
    TransferStats busAfter{};
  } _run{};
  TMP1x2::Sample _lastSample{};
  bool _hasSample = false;
};
}  // namespace tmp1x2_cli
