#include "ConsoleBuffer.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>

static unsigned failures = 0;
#define CHECK(expr) do { if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (false)

struct Sink {
  std::string text;
  size_t accepted = 256;
  size_t largestRequest = 0;
  unsigned calls = 0;
  static size_t write(void* context, const char* data, size_t size) {
    auto& sink = *static_cast<Sink*>(context);
    ++sink.calls;
    sink.largestRequest = std::max(size, sink.largestRequest);
    const size_t count = std::min(size, sink.accepted);
    sink.text.append(data, count);
    return count;
  }
};

template <size_t N>
static void format(tmp1x2_cli::ConsoleBuffer<N>& queue, const char* text, ...) {
  va_list args;
  va_start(args, text);
  queue.format(text, args);
  va_end(args);
}

int main() {
  using tmp1x2_cli::ConsoleBuffer;
  ConsoleBuffer<64> queue;
  Sink sink;
  const std::string first(50, 'a');
  const std::string second(40, 'b');
  queue.append(first.data(), first.size());
  sink.accepted = 0;
  queue.drain(Sink::write, &sink);
  CHECK(sink.calls == 1 && queue.stats().queuedBytes == 50);
  CHECK(queue.stats().sentBytes == 0 && queue.stats().droppedBytes == 0);
  sink.accepted = 30;
  queue.drain(Sink::write, &sink);
  CHECK(sink.calls == 2 && queue.stats().queuedBytes == 20);
  queue.append(second.data(), second.size()); // Exercise wrap with unread data.
  for (unsigned i = 0; i < 10 && queue.stats().queuedBytes; ++i) queue.drain(Sink::write, &sink, 17);
  CHECK(sink.text == first + second);
  CHECK(queue.stats().queuedBytes == 0 && queue.stats().sentBytes == 90);

  ConsoleBuffer<64> full;
  Sink recovery;
  const std::string filled(64, 'x');
  full.append(filled.data(), filled.size());
  full.append("must not appear", 15);
  CHECK(full.stats().droppedBytes == 15 && full.stats().queuedBytes == 64);
  full.drain(Sink::write, &recovery); // Existing output stays before the loss report.
  CHECK(recovery.text == filled);
  full.drain(Sink::write, &recovery);
  CHECK(recovery.text.find("CONSOLE_OUTPUT_LOST") != std::string::npos);
  CHECK(recovery.text.find("must not appear") == std::string::npos);
  CHECK(full.stats().droppedBytes == 15 && full.stats().truncatedMessages == 0);

  ConsoleBuffer<> formatted;
  Sink formattedSink;
  const std::string oversized(800, 'q');
  format(formatted, "%s", oversized.c_str());
  CHECK(formatted.stats().droppedBytes == 800);
  CHECK(formatted.stats().truncatedMessages == 1);
  format(formatted, "value=%u\n", 123U);
  formatted.drain(Sink::write, &formattedSink);
  CHECK(formattedSink.text.find("value=123\n") != std::string::npos);
  CHECK(formattedSink.text.find("CONSOLE_OUTPUT_LOST") != std::string::npos);
  CHECK(formattedSink.text.find('q') == std::string::npos);

  ConsoleBuffer<> burst;
  Sink bounded;
  const std::string huge(10000, 'z');
  burst.append(huge.data(), huge.size());
  burst.drain(Sink::write, &bounded);
  CHECK(bounded.calls == 1 && bounded.largestRequest == 256);
  CHECK(burst.stats().queuedBytes == huge.size() - 256);
  burst.drain(Sink::write, &bounded, 0);
  CHECK(bounded.calls == 1);
  CHECK(tmp1x2_cli::consoleSaturatingAdd(UINT32_MAX - 2, 1) == UINT32_MAX - 1);
  CHECK(tmp1x2_cli::consoleSaturatingAdd(UINT32_MAX - 2, 3) == UINT32_MAX);
  CHECK(tmp1x2_cli::consoleSaturatingAdd(0, std::numeric_limits<size_t>::max()) == UINT32_MAX);
  std::printf("Console queue: %u failures\n", failures);
  return failures ? 1 : 0;
}
