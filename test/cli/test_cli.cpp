#include "Tmp1x2Cli.h"
#include <cstdio>
#include <cstring>
#include <string>

struct Fixture {
  std::string output;
  unsigned transfers = 0;
  unsigned writes = 0;
  uint32_t timeMs = 100;
  uint32_t readyAtMs = 0;
  uint8_t lastAddress = 0;
  unsigned readFailures = 0;
  unsigned writeFailures = 0;
  bool pending = false;
  bool stuck = false;
  bool pinLevel = false;
  unsigned gpioReads = 0;
  unsigned probed[128]{};
  tmp1x2_cli::TransferStats adapter{};
  uint16_t regs[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  static uint32_t now(void* user) { return static_cast<Fixture*>(user)->timeMs; }
  static void yield(void* user) { ++static_cast<Fixture*>(user)->timeMs; }
  void settle(tmp1x2_cli::Cli& cli) {
    for (unsigned i = 0; i < 1200; ++i) { ++timeMs; cli.tick(); }
  }
  static tmp1x2_cli::TransferStats stats(void* user) { return static_cast<Fixture*>(user)->adapter; }
  static void resetStats(void* user) { static_cast<Fixture*>(user)->adapter = tmp1x2_cli::TransferStats{}; }
  static bool gpio(int, void* user) { auto& f = *static_cast<Fixture*>(user); ++f.gpioReads; return f.pinLevel; }
  static TMP1x2::Status probe(uint8_t address, void* user) {
    auto& f = *static_cast<Fixture*>(user); ++f.transfers; ++f.probed[address]; f.adapter.record(true);
    return TMP1x2::Status::Ok();
  }
  TMP1x2::Config config() {
    TMP1x2::Config c;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cUser = this;
    c.nowMs = now; c.cooperativeYield = yield; c.timeUser = this;
    return c;
  }
  tmp1x2_cli::Platform platform() {
    tmp1x2_cli::Platform p;
    p.vprintf = print; p.nowMs = now; p.user = this;
    p.transferStats = stats; p.resetTransferStats = resetStats; p.probeAddress = probe;
    return p;
  }
  static void print(void* user, const char* format, va_list args) {
    char text[2048]; std::vsnprintf(text, sizeof(text), format, args);
    static_cast<Fixture*>(user)->output += text;
  }
  static TMP1x2::Status write(uint8_t address, const uint8_t* data, size_t n, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user); ++f.transfers; ++f.writes;
    f.lastAddress = address;
    if (f.writeFailures) {
      --f.writeFailures;
      f.adapter.record(false);
      return TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "injected write failure");
    }
    f.adapter.record(true);
    if (n != 3 || data[0] < 1 || data[0] > 3) return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "framing");
    auto word = static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8U) | data[2]);
    f.regs[data[0]] = data[0] == 1 ? static_cast<uint16_t>((word & 0x1FD0U) | 0x6020U) : word;
    if (data[0] == 1) {
      f.pending = (word & 0x8100U) == 0x8100U;
      if (f.pending) f.readyAtMs = f.timeMs + TMP1x2::cmd::CONVERSION_TIME_MAX_MS;
      else if (word & 0x0100U) f.regs[1] |= 0x8000U;
    }
    return TMP1x2::Status::Ok();
  }
  static TMP1x2::Status read(uint8_t address, const uint8_t* data, size_t n, uint8_t* out, size_t count, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user); ++f.transfers;
    f.lastAddress = address;
    if (f.readFailures) {
      --f.readFailures;
      f.adapter.record(false);
      return TMP1x2::Status::Error(TMP1x2::Err::I2C_ERROR, "injected read failure");
    }
    f.adapter.record(true);
    if (n != 1 || count != 2 || data[0] > 3) return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "framing");
    if (f.pending && !f.stuck && static_cast<int32_t>(f.timeMs - f.readyAtMs) >= 0) {
      f.pending = false; f.regs[1] |= 0x8000U;
      f.regs[0] = (f.regs[1] & 0x10U) != 0 ? 0x0C81 : 0x1900;
    }
    out[0] = static_cast<uint8_t>(f.regs[data[0]] >> 8U); out[1] = static_cast<uint8_t>(f.regs[data[0]]);
    return TMP1x2::Status::Ok();
  }
};
#define CHECK(x) do { if (!(x)) { std::printf("CLI check failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
  Fixture f; tmp1x2_cli::Cli cli; auto p = f.platform(); auto c = f.config();
  cli.setup(p, c); CHECK(f.transfers == 0); f.settle(cli); f.output.clear();
  cli.processCommand("help");
  CHECK(f.output.find("\033[36m=== TMP1x2 CLI Help ===\033[0m") != std::string::npos);
  CHECK(f.output.find("\033[32m[Configuration]\033[0m") != std::string::npos);
  CHECK(f.output.find("%-32") == std::string::npos);
  CHECK(f.output.find("readblocking") != std::string::npos);
  const unsigned traffic = f.transfers;
  for (const char* cmd : {"health", "settings", "version", "sample", "sampleage", "diag", "stats", "xfer_stats", "counters", "timing", "job", "result", "convert 0x1900", "thcalc 20.01"}) cli.processCommand(cmd);
  CHECK(f.transfers == traffic);
  for (const char* cmd : {"wreg 1 0x10000", "wreg 0 1", "wreg 1 -1", "reg 4", "reg 0 extra", "threshold nan 80", "threshold 1 inf", "rate 9", "faults 3", "addr 0x40", "watch -1", "extended 2", "mode potato"}) {
    cli.processCommand(cmd); CHECK(f.transfers == traffic);
  }
  cli.processCommand("color off"); f.output.clear(); cli.processCommand("help");
  CHECK(f.output.find('\033') == std::string::npos);
  f.output.clear(); cli.processCommand("read"); CHECK(f.output.find("25.0000") != std::string::npos || f.output.find("25.000") != std::string::npos);
  const unsigned before = f.transfers;
  const std::string tooLong = "wreg 1 0x6100 " + std::string(180, ' ');
  for (char ch : tooLong) cli.feed(ch);
  cli.feed('\n'); CHECK(f.transfers == before);
  cli.processCommand("end"); CHECK(f.transfers == before);
  cli.processCommand("addr 0x49"); CHECK(f.transfers == before);
  // Address selection is a bus-silent binding change. Recovery must use the
  // target shown by settings, including after rejected uninitialized setters.
  cli.processCommand("shutdown");
  cli.processCommand("threshold 1 2");
  cli.processCommand("rate 1");
  CHECK(f.transfers == before);
  cli.processCommand("recover");
  f.settle(cli);
  CHECK(f.lastAddress == 0x49);
  f.output.clear(); cli.processCommand("settings");
  CHECK(f.output.find("address=0x49") != std::string::npos);
  // A rejected operation after unbind must not erase application callbacks.
  cli.processCommand("unbind");
  cli.processCommand("shutdown");
  cli.processCommand("mode shutdown");
  cli.processCommand("threshold 1 2");
  f.output.clear(); cli.processCommand("begin");
  f.settle(cli);
  CHECK(f.output.find("[I] OK") != std::string::npos);
  CHECK(f.lastAddress == 0x49);
  // Exercise the actual character input path, including tab-separated tokens.
  f.output.clear();
  for (char ch : std::string("reg\t0\r\n")) cli.feed(ch);
  CHECK(f.output.find("0x00 = 0x1900") != std::string::npos);
  const unsigned beforeMalformed = f.transfers;
  for (char control : {'\0', '\x01', '\x18', '\x1B'}) {
    std::string malformed = "w";
    malformed.push_back(control); malformed += "reg 1 0x6100\n";
    for (char ch : malformed) cli.feed(ch);
    CHECK(f.transfers == beforeMalformed);
  }
  CHECK(f.output.find("Invalid control character") != std::string::npos);
  // An invalid line does not poison the next line; backspace still edits.
  f.output.clear(); for (char ch : std::string("reg x\b0\n")) cli.feed(ch);
  CHECK(f.output.find("0x00 = 0x1900") != std::string::npos);
  f.output.clear(); cli.processCommand("watch 08 1"); cli.processCommand("stop");
  CHECK(f.output.find("Watch: 8 samples") != std::string::npos);
  f.output.clear(); cli.processCommand("watch 010 1"); cli.processCommand("stop");
  CHECK(f.output.find("Watch: 10 samples") != std::string::npos);
  // Ambiguous writes retain the new desired setting and dirty diagnosis.
  f.writeFailures = 1;
  cli.processCommand("rate 1");
  f.settle(cli);
  f.output.clear(); cli.processCommand("settings"); cli.processCommand("health");
  CHECK(f.output.find("rate=1 Hz") != std::string::npos);
  CHECK(f.output.find("Dirty reason: I2C_ERROR") != std::string::npos);
  cli.processCommand("recover");
  f.settle(cli);
  {
    Fixture watch; tmp1x2_cli::Cli shell;
    shell.setup(watch.platform(), watch.config()); watch.settle(shell); shell.processCommand("color off");
    shell.processCommand("mode shutdown"); watch.settle(shell);
    shell.processCommand("watch 2 1"); shell.tick();
    watch.timeMs += TMP1x2::cmd::CONVERSION_TIME_MAX_MS;
    watch.readFailures = 1; shell.tick();
    ++watch.timeMs; shell.tick();
    CHECK(watch.output.find("Watch stopped: ok=1 fail=1") != std::string::npos);
    CHECK(watch.output.find("BUSY") == std::string::npos);
    // Stop is bus-silent even if a one-shot has not completed. Another watch
    // joins that conversion without triggering a replacement write.
    shell.processCommand("watch 1 1"); shell.tick();
    const unsigned afterStart = watch.transfers;
    const unsigned writesAfterStart = watch.writes;
    shell.processCommand("stop");
    watch.timeMs += 1000;
    for (unsigned i = 0; i < 5; ++i) shell.tick();
    CHECK(watch.transfers == afterStart);
    watch.output.clear(); shell.processCommand("watch 1 1"); shell.tick();
    CHECK(watch.writes == writesAfterStart);
    CHECK(watch.output.find("Watch stopped: ok=1 fail=0") != std::string::npos);
    // A stuck conversion stops at its deadline and does not silently resume
    // physical polling on subsequent idle ticks.
    watch.stuck = true; watch.output.clear();
    shell.processCommand("watch 1 1"); shell.tick();
    watch.timeMs += 500; shell.tick();
    CHECK(watch.output.find("watch conversion deadline") != std::string::npos);
    const unsigned afterTimeout = watch.transfers;
    for (unsigned i = 0; i < 5; ++i) { ++watch.timeMs; shell.tick(); }
    CHECK(watch.transfers == afterTimeout);
    // Explicit tryread can still consume the retained conversion.
    watch.stuck = false; watch.output.clear(); shell.processCommand("tryread");
    CHECK(watch.output.find("Temperature:") != std::string::npos);
    // Manual start is also explicit: idle ticks cannot add hidden I2C.
    shell.processCommand("start");
    const unsigned afterManualStart = watch.transfers;
    watch.timeMs += 100;
    shell.tick(); CHECK(watch.transfers == afterManualStart);
    shell.processCommand("tryread"); CHECK(watch.transfers > afterManualStart);
  }
  {
    Fixture unclocked; tmp1x2_cli::Cli shell;
    auto cfg = unclocked.config(); cfg.nowMs = nullptr;
    shell.setup(unclocked.platform(), cfg);
    unclocked.settle(shell);
    const unsigned initial = unclocked.transfers;
    unclocked.output.clear(); shell.processCommand("watch 1 1"); shell.tick();
    CHECK(unclocked.transfers == initial);
    CHECK(unclocked.output.find("watch requires") != std::string::npos);
  }
  {
    Fixture normalized; tmp1x2_cli::Cli shell;
    auto cfg = normalized.config(); cfg.lowThresholdC = 1.01f; cfg.offlineThreshold = 0;
    shell.setup(normalized.platform(), cfg);
    normalized.settle(shell);
    normalized.output.clear(); shell.processCommand("settings");
    CHECK(normalized.output.find("low=1.0000 C") != std::string::npos);
    CHECK(normalized.output.find("offline-threshold=1") != std::string::npos);
  }
  {
    Fixture summary; tmp1x2_cli::Cli shell;
    shell.setup(summary.platform(), summary.config()); summary.settle(shell);
    shell.processCommand("color off"); shell.processCommand("quiet on"); summary.output.clear();
    shell.processCommand("watch 4 10"); shell.tick();
    summary.timeMs += 10; summary.regs[0] = 0x1E00; shell.tick();
    summary.timeMs += 10; summary.regs[0] = 0x1400; shell.tick();
    summary.timeMs += 10; summary.readFailures = 1; shell.tick();
    CHECK(summary.output.find("Temperature:") == std::string::npos);
    CHECK(summary.output.find("ok=3 fail=1 elapsed=30 ms") != std::string::npos);
    CHECK(summary.output.find("min=20.0000 C max=30.0000 C mean=25.0000 C") != std::string::npos);
    CHECK(summary.output.find("Tracked health delta: ok=3 fail=1") != std::string::npos);
    CHECK(summary.output.find("Adapter delta: attempts=4 ok=3 fail=1") != std::string::npos);
    CHECK(summary.output.find("First error: [E] I2C_ERROR") != std::string::npos);
    CHECK(summary.output.find("Last error: [E] I2C_ERROR") != std::string::npos);
    summary.output.clear(); shell.processCommand("sample");
    CHECK(summary.output.find("20.0000 C") != std::string::npos);
    summary.output.clear(); shell.processCommand("health");
    const auto health = summary.output.substr(0, summary.output.find('\n'));
    const unsigned transfers = summary.transfers;
    shell.processCommand("xfer_reset"); CHECK(summary.adapter.attempts == 0);
    summary.output.clear(); shell.processCommand("health");
    CHECK(summary.output.substr(0, summary.output.find('\n')) == health);
    shell.processCommand("stats");
    CHECK(summary.output.find("Adapter delta: attempts=4 ok=3 fail=1") != std::string::npos);
    CHECK(summary.transfers == transfers);
    shell.processCommand("stats reset"); summary.output.clear(); shell.processCommand("stats");
    CHECK(summary.output.find("No watch/stress run statistics") != std::string::npos);
    shell.processCommand("watch 2 10"); shell.tick();
    const uint32_t beforeReset = summary.adapter.attempts;
    shell.processCommand("xfer_reset"); CHECK(summary.adapter.attempts == beforeReset);
    shell.processCommand("stop");
  }
  {
    Fixture raw; tmp1x2_cli::Cli shell;
    shell.setup(raw.platform(), raw.config()); raw.settle(shell); shell.processCommand("color off");
    shell.processCommand("end"); raw.output.clear(); shell.processCommand("health");
    const auto health = raw.output.substr(0, raw.output.find('\n'));
    const unsigned transfers = raw.transfers;
    shell.processCommand("rawread 0"); shell.processCommand("rawdump"); shell.processCommand("rawwrite 2 0x1400");
    CHECK(raw.transfers == transfers + 6);
    CHECK(raw.regs[2] == 0x1400);
    raw.output.clear(); shell.processCommand("health");
    CHECK(raw.output.substr(0, raw.output.find('\n')) == health);
    CHECK(raw.output.find("dirty=yes") != std::string::npos);
    const unsigned beforeInvalid = raw.transfers;
    for (const char* command : {"reg 0", "rawread 4", "rawwrite 0 1", "rawwrite 1 65536", "rawdump extra", "convert 65536", "thcalc nan"})
      shell.processCommand(command);
    CHECK(raw.transfers == beforeInvalid);
    raw.output.clear(); shell.processCommand("convert 0x1900"); shell.processCommand("thcalc 20.01 normal");
    CHECK(raw.output.find("Decoded: 25.0000 C") != std::string::npos);
    CHECK(raw.output.find("Threshold: 0x1400 = 20.0000 C") != std::string::npos);
    shell.processCommand("thdecode 0xF600 normal");
    CHECK(raw.output.find("Threshold: 0xF600 = -10.0000 C") != std::string::npos);
    CHECK(raw.transfers == beforeInvalid);
    shell.processCommand("unbind"); shell.processCommand("rawread 0"); CHECK(raw.transfers == beforeInvalid);
  }
  {
    Fixture mixed; tmp1x2_cli::Cli shell;
    shell.setup(mixed.platform(), mixed.config()); mixed.settle(shell);
    shell.processCommand("color off"); shell.processCommand("quiet on");
    const unsigned beforeInvalid = mixed.transfers;
    for (const char* command : {"stress_mix 0", "stress_mix -1", "stress_mix 100001", "stress_mix 1 10", "stress_mix potato"})
      shell.processCommand(command);
    CHECK(mixed.transfers == beforeInvalid);
    const unsigned beforeWrites = mixed.writes;
    mixed.output.clear(); shell.processCommand("stress_mix 2");
    // One probe, one configuration read, one three-read threshold operation,
    // and one temperature read; phases remain independently cancellable.
    for (unsigned phase = 0; phase < 4; ++phase) {
      const unsigned beforeTick = mixed.transfers;
      shell.tick(); CHECK(mixed.transfers - beforeTick == (phase == 2 ? 3U : 1U));
      ++mixed.timeMs;
    }
    mixed.timeMs += 100; mixed.regs[0] = 0x1E00;
    for (unsigned phase = 0; phase < 4; ++phase) { shell.tick(); ++mixed.timeMs; }
    CHECK(mixed.writes == beforeWrites);
    CHECK(mixed.output.find("Mixed stress stopped: ok=8 fail=0") != std::string::npos);
    CHECK(mixed.output.find("target=2 completed=2") != std::string::npos);
    CHECK(mixed.output.find("Mixed checks: completed=8 samples=2") != std::string::npos);
    CHECK(mixed.output.find("min=25.0000 C max=30.0000 C mean=27.5000 C") != std::string::npos);
    CHECK(mixed.output.find("Tracked health delta: ok=10 fail=0") != std::string::npos);
    CHECK(mixed.output.find("Adapter delta: attempts=12 ok=12 fail=0") != std::string::npos);
    CHECK(mixed.output.find("Temperature:") == std::string::npos);
    // Probe failures affect adapter/run statistics without affecting health.
    mixed.output.clear(); shell.processCommand("stress_mix 1"); mixed.readFailures = 1;
    for (unsigned phase = 0; phase < 4; ++phase) { shell.tick(); ++mixed.timeMs; }
    CHECK(mixed.output.find("Mixed stress stopped: ok=3 fail=1") != std::string::npos);
    CHECK(mixed.output.find("Tracked health delta: ok=5 fail=0") != std::string::npos);
    CHECK(mixed.output.find("Adapter delta: attempts=6 ok=5 fail=1") != std::string::npos);
    CHECK(mixed.output.find("samples=1") != std::string::npos);
    CHECK(mixed.output.find("mean=30.0000 C") != std::string::npos);
    // Stop after a non-sample phase emits no more bus traffic on future ticks.
    shell.processCommand("stress_mix 10"); shell.tick();
    const unsigned beforeStop = mixed.transfers; shell.processCommand("stop"); mixed.settle(shell);
    CHECK(mixed.transfers == beforeStop);
    // A failed threshold read is not a sample and cannot skew the mean.
    mixed.output.clear(); shell.processCommand("stress_mix 1"); shell.tick(); shell.tick();
    mixed.readFailures = 1; shell.tick(); shell.tick();
    CHECK(mixed.output.find("Mixed stress stopped: ok=3 fail=1") != std::string::npos);
    CHECK(mixed.output.find("samples=1") != std::string::npos);
    CHECK(mixed.output.find("mean=30.0000 C") != std::string::npos);
    shell.processCommand("mode shutdown"); mixed.settle(shell);
    mixed.output.clear(); shell.processCommand("stress_mix 1");
    for (unsigned phase = 0; phase < 4; ++phase) { shell.tick(); ++mixed.timeMs; }
    // The sample phase has started a one-shot and is still awaiting completion.
    CHECK(mixed.output.find("Mixed stress stopped") == std::string::npos);
    const unsigned pendingTransfers = mixed.transfers; const unsigned pendingWrites = mixed.writes;
    shell.processCommand("stop"); mixed.timeMs += 100; shell.tick(); CHECK(mixed.transfers == pendingTransfers);
    mixed.output.clear(); shell.processCommand("stress_mix 1");
    for (unsigned phase = 0; phase < 4; ++phase) { shell.tick(); ++mixed.timeMs; }
    CHECK(mixed.writes == pendingWrites); // Join the retained conversion.
    CHECK(mixed.output.find("Mixed stress stopped: ok=4 fail=0") != std::string::npos);
    CHECK(mixed.output.find("samples=1") != std::string::npos);
    mixed.stuck = true; mixed.output.clear(); shell.processCommand("stress_mix 1");
    for (unsigned phase = 0; phase < 4; ++phase) { shell.tick(); ++mixed.timeMs; }
    mixed.timeMs += 500; shell.tick();
    CHECK(mixed.output.find("mixed sample deadline") != std::string::npos);
    CHECK(mixed.output.find("Mixed checks: completed=4 samples=0") != std::string::npos);
    CHECK(mixed.output.find("Temperature summary:") == std::string::npos);
    const unsigned timeoutTransfers = mixed.transfers; mixed.settle(shell); CHECK(mixed.transfers == timeoutTransfers);
  }
  {
    Fixture family; tmp1x2_cli::Cli shell; auto cfg = family.config();
    cfg.alertPin = 21; cfg.gpioRead = Fixture::gpio; cfg.gpioUser = &family;
    shell.setup(family.platform(), cfg); family.settle(shell); shell.processCommand("color off");
    const unsigned beforeGpio = family.transfers;
    family.output.clear(); shell.processCommand("alertpin");
    CHECK(family.output.find("Physical ALERT: active") != std::string::npos);
    CHECK(family.transfers == beforeGpio && family.gpioReads == 1);
    family.pinLevel = true; shell.processCommand("intpin");
    CHECK(family.output.find("Physical ALERT: inactive") != std::string::npos);
    shell.processCommand("discover");
    for (unsigned address = 0; address < 128; ++address)
      CHECK(family.probed[address] == ((address >= 0x40 && address <= 0x43) || (address >= 0x48 && address <= 0x4B) ? 1U : 0U));
    shell.processCommand("end"); const unsigned afterEnd = family.transfers;
    shell.processCommand("model tmp112d"); shell.processCommand("addr 0x43");
    CHECK(family.transfers == afterEnd);
    family.output.clear(); shell.processCommand("settings");
    CHECK(family.output.find("model=TMP112D_ADDRESS_SELECT address=0x43") != std::string::npos);
    CHECK(family.output.find("physical ALERT=not present pin=-1") != std::string::npos);
    shell.processCommand("begin"); family.settle(shell); CHECK(family.lastAddress == 0x43);
    const unsigned beforeUnavailablePin = family.gpioReads;
    shell.processCommand("alertpin"); CHECK(family.gpioReads == beforeUnavailablePin);
    shell.processCommand("end"); shell.processCommand("model tmp112");
    shell.processCommand("begin"); family.settle(shell);
    family.output.clear(); shell.processCommand("alertpin");
    CHECK(family.output.find("pin=21") != std::string::npos);
    CHECK(family.lastAddress == 0x48);
  }
  {
    Fixture owner; tmp1x2_cli::Cli shell;
    shell.setup(owner.platform(), owner.config()); CHECK(owner.transfers == 0);
    // Input remains useful while initialization is admitted but unpolled.
    shell.processCommand("color off"); owner.output.clear(); shell.processCommand("job");
    CHECK(owner.output.find("active=yes") != std::string::npos);
    for (const char* command : {"health", "settings", "job status", "rawread 0", "scan"}) shell.processCommand(command);
    CHECK(owner.transfers == 0);
    shell.processCommand("cancel"); CHECK(owner.transfers == 0);
    owner.output.clear(); shell.processCommand("result");
    CHECK(owner.output.find("CANCELLED") != std::string::npos);
    shell.processCommand("begin");
    for (unsigned i = 0; i < 1200; ++i) {
      const unsigned beforeTick = owner.transfers;
      ++owner.timeMs; shell.tick(); CHECK(owner.transfers - beforeTick <= 1);
    }
    CHECK(owner.output.find("[I] OK") != std::string::npos);
    const unsigned beforeConfigure = owner.transfers;
    shell.processCommand("extended on"); CHECK(owner.transfers == beforeConfigure);
    owner.output.clear(); shell.processCommand("job");
    CHECK(owner.output.find("Staged:") != std::string::npos);
    CHECK(owner.output.find("extended=yes") != std::string::npos);
    // Cancel before any transport keeps old desired format and performs no I2C.
    shell.processCommand("cancel"); CHECK(owner.transfers == beforeConfigure);
    owner.output.clear(); shell.processCommand("settings");
    CHECK(owner.output.find("extended=no") != std::string::npos);
    shell.processCommand("extended on");
    // Poll until this operation has attempted a write, then cancel during
    // settling; the input command itself must not advance or block on I2C.
    const unsigned baselineWrites = owner.writes;
    for (unsigned i = 0; i < 10 && owner.writes == baselineWrites; ++i) { ++owner.timeMs; shell.tick(); }
    const unsigned beforeCancel = owner.transfers;
    shell.processCommand("stop"); CHECK(owner.transfers == beforeCancel);
    owner.output.clear(); shell.processCommand("health");
    CHECK(owner.output.find("dirty=yes") != std::string::npos);
    shell.processCommand("recover"); owner.settle(shell);
    owner.output.clear(); shell.processCommand("read"); CHECK(owner.output.find("25.0000 C") != std::string::npos);
    shell.processCommand("measure"); owner.settle(shell);
    owner.output.clear(); const unsigned afterRead = owner.transfers; shell.processCommand("result");
    CHECK(owner.output.find("kind=READ") != std::string::npos);
    CHECK(owner.output.find("Result sample: 25.0000 C") != std::string::npos);
    CHECK(owner.transfers == afterRead);
  }
  std::puts("CLI parsing, diagnostics, raw access, model families, sampling summaries and cooperative workflow checks passed");
  return 0;
}
