#include "dex/infrastructure/shared_memory/shared_memory_private.h"

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace dex::shared_memory::detail {

std::string FormatSystemError(std::string_view prefix, int error_code) {
  constexpr std::size_t kErrorBufferSize = 256;
  std::array<char, kErrorBufferSize> buffer = {};
  const char* error_string = strerror_r(error_code, buffer.data(), buffer.size());
  return std::string(prefix) + ": " + error_string;
}

}  // namespace dex::shared_memory::detail

