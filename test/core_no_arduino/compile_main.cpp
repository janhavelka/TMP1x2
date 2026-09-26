#include <type_traits>
#define INTERRUPT 226 // Xtensa register macro must not collide with public enums.
#define LOW 0x0       // Arduino GPIO macros also precede includes in consumers.
#define HIGH 0x1
#include "TMP1x2/TMP1x2.h"
#include "TMP1x2/BusOperations.h"
#include "TMP1x2/CommandTable.h"
#include "TMP1x2/Version.h"
static_assert(std::is_trivially_copyable<TMP1x2::Status>::value, "POD status");
static_assert(std::is_trivially_copyable<TMP1x2::Sample>::value, "POD sample");
static_assert(std::is_trivially_copyable<TMP1x2::OperationResult>::value, "fixed-memory operation result");
static_assert(std::is_trivially_copyable<TMP1x2::OperationSnapshot>::value, "fixed-memory operation snapshot");
static_assert(std::is_trivially_copyable<TMP1x2::PollResult>::value, "fixed-memory poll result");
static_assert(TMP1x2::isValidAddress(TMP1x2::Model::TMP112D_ADDRESS_SELECT, 0x40), "package capabilities");
int main() { TMP1x2::TMP1x2 sensor; sensor.end(); return 0; }
