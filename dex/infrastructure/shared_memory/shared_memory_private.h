#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_PRIVATE_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_PRIVATE_H

#include <string>

namespace dex::shared_memory::detail {

/**
 * Formats a system error message with a prefix.
 * @param prefix The prefix to prepend to the error message
 * @param err The system error code (errno)
 * @return A formatted string containing the prefix and system error message
 */
[[nodiscard]] std::string FormatSystemError(std::string_view prefix, int err);

}  // namespace dex::shared_memory::detail
#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_PRIVATE_H

