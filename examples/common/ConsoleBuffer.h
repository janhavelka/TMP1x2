#pragma once

// Example-only, single-owner console queue. Formatting never waits for a host.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace tmp1x2_cli {
struct ConsoleStats {
  uint32_t queuedBytes = 0;
  uint32_t sentBytes = 0;  // Accepted by the backend, not an acknowledgement from the host.
  uint32_t droppedBytes = 0;
  uint32_t truncatedMessages = 0;
  const char* backend = "unspecified";
};

inline uint32_t consoleSaturatingAdd(uint32_t value, size_t increment) {
  return increment > UINT32_MAX - value ? UINT32_MAX : value + static_cast<uint32_t>(increment);
}

template <size_t Capacity = 16384>
class ConsoleBuffer {
 public:
  using WriteSome = size_t (*)(void*, const char*, size_t);
  static_assert(Capacity >= 64 && Capacity <= UINT32_MAX, "console queue size out of range");

  void append(const char* data, size_t length) {
    if (length > Capacity - _used) { lost(length); return; }
    enqueue(data, length);
  }

  void format(const char* format, va_list args) {
    char buffer[512];
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(buffer)) {
      _stats.truncatedMessages = consoleSaturatingAdd(_stats.truncatedMessages, 1);
      // Drop the whole record instead of presenting a truncated diagnostic.
      lost(length > 0 ? static_cast<size_t>(length) : 0);
      return;
    }
    append(buffer, static_cast<size_t>(length));
  }

  // At most one nonblocking backend call, capped at budget bytes. Short writes
  // and unavailable transports leave the remaining bytes in their original order.
  void drain(WriteSome write, void* user = nullptr, size_t budget = 256) {
    static constexpr char loss[] = "\n[E] CONSOLE_OUTPUT_LOST; inspect console counters\n";
    if (_lossPending && Capacity - _used >= sizeof(loss) - 1) {
      enqueue(loss, sizeof(loss) - 1);
      _lossPending = false;
    }
    if (!write || _used == 0 || budget == 0) return;
    size_t count = _used < Capacity - _head ? _used : Capacity - _head;
    if (count > budget) count = budget;
    size_t sent = write(user, _data + _head, count);
    if (sent > count) sent = count;
    _head = (_head + sent) % Capacity;
    _used -= sent;
    _stats.sentBytes = consoleSaturatingAdd(_stats.sentBytes, sent);
  }

  ConsoleStats stats() const {
    ConsoleStats result = _stats;
    result.queuedBytes = static_cast<uint32_t>(_used);
    return result;
  }

 private:
  void lost(size_t length) {
    _stats.droppedBytes = consoleSaturatingAdd(_stats.droppedBytes, length);
    _lossPending = true;
  }
  void enqueue(const char* data, size_t length) {
    const size_t tail = (_head + _used) % Capacity;
    const size_t first = length < Capacity - tail ? length : Capacity - tail;
    if (first != 0) std::memcpy(_data + tail, data, first);
    if (length != first) std::memcpy(_data, data + first, length - first);
    _used += length;
  }
  char _data[Capacity]{};
  size_t _head = 0;
  size_t _used = 0;
  ConsoleStats _stats{};
  bool _lossPending = false;
};
}  // namespace tmp1x2_cli
