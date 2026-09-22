#include <type_traits>
#define INTERRUPT 226 // Xtensa register macro must not collide with public enums.
#include "TMP1x2/TMP1x2.h"
#include "TMP1x2/CommandTable.h"
#include "TMP1x2/Version.h"
static_assert(std::is_trivially_copyable<TMP1x2::Status>::value, "POD status");
static_assert(std::is_trivially_copyable<TMP1x2::Sample>::value, "POD sample");
int main() { TMP1x2::TMP1x2 sensor; sensor.end(); return 0; }
