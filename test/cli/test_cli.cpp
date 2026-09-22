#include "Tmp1x2Cli.h"
#include <cstdio>
#include <cstring>
#include <string>

struct Fixture {
  std::string output;
  unsigned transfers = 0;
  unsigned writes = 0;
  uint16_t regs[4] = {0x1900, 0x60A0, 0x4B00, 0x5000};
  static void print(void* user, const char* format, va_list args) {
    char text[2048]; std::vsnprintf(text, sizeof(text), format, args);
    static_cast<Fixture*>(user)->output += text;
  }
  static TMP1x2::Status write(uint8_t, const uint8_t* data, size_t n, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user); ++f.transfers; ++f.writes;
    if (n != 3 || data[0] < 1 || data[0] > 3) return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "framing");
    auto word = static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8U) | data[2]);
    f.regs[data[0]] = data[0] == 1 ? static_cast<uint16_t>((word & 0x1FD0U) | 0x6020U) : word;
    return TMP1x2::Status::Ok();
  }
  static TMP1x2::Status read(uint8_t, const uint8_t* data, size_t n, uint8_t* out, size_t count, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user); ++f.transfers;
    if (n != 1 || count != 2 || data[0] > 3) return TMP1x2::Status::Error(TMP1x2::Err::INVALID_PARAM, "framing");
    out[0] = static_cast<uint8_t>(f.regs[data[0]] >> 8U); out[1] = static_cast<uint8_t>(f.regs[data[0]]);
    return TMP1x2::Status::Ok();
  }
};
#define CHECK(x) do { if (!(x)) { std::printf("CLI check failed line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
  Fixture f; tmp1x2_cli::Cli cli; tmp1x2_cli::Platform p; p.vprintf = Fixture::print; p.user = &f;
  TMP1x2::Config c; c.i2cWrite = Fixture::write; c.i2cWriteRead = Fixture::read; c.i2cUser = &f;
  cli.setup(p, c); f.output.clear();
  cli.processCommand("help");
  CHECK(f.output.find("\033[36m=== TMP1x2 CLI Help ===\033[0m") != std::string::npos);
  CHECK(f.output.find("\033[32m[Configuration]\033[0m") != std::string::npos);
  CHECK(f.output.find("%-32") == std::string::npos);
  CHECK(f.output.find("readblocking") != std::string::npos);
  const unsigned traffic = f.transfers;
  for (const char* cmd : {"health", "settings", "version", "sample", "sampleage", "diag"}) cli.processCommand(cmd);
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
  std::puts("CLI help, colors, passive diagnostics, strict parsers and overflow checks passed");
  return 0;
}
