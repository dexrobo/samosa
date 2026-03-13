#include "dex/infrastructure/shared_memory/shared_memory_private.h"

#include <array>
#include <cstring>

namespace dex::shared_memory::detail {

std::string FormatSystemError(std::string_view prefix, int error_code) {
  std::array<char, 256> buffer = {};
  const char* error_string = strerror_r(error_code, buffer.data(), buffer.size());
  return std::string(prefix) + ": " + error_string;
}

}  // namespace dex::shared_memory::detail
