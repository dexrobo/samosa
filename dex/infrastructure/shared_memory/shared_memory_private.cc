#include "dex/infrastructure/shared_memory/shared_memory_private.h"

#include <array>
#include <cstring>

namespace dex::shared_memory::detail {

std::string FormatSystemError(std::string_view prefix, int error_code) {
  std::array<char, 256> buffer = {};
#ifdef __APPLE__
  const int result = strerror_r(error_code, buffer.data(), buffer.size());
  if (result == 0) {
    return std::string(prefix) + ": " + buffer.data();
  }
  return std::string(prefix) + ": Unknown error " + std::to_string(error_code);
#else
  const char* error_string = strerror_r(error_code, buffer.data(), buffer.size());
  return std::string(prefix) + ": " + error_string;
#endif
}

}  // namespace dex::shared_memory::detail
