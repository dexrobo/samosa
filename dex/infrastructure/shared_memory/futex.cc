#include "dex/infrastructure/shared_memory/futex.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory_private.h"

namespace dex::shared_memory::detail {

////////////////////////////////////////////////////////////////////////////////
/// Futex helper functions (Linux-specific) for conditional synchronization.
////////////////////////////////////////////////////////////////////////////////
// Wait on the futex at addr until *addr != expected.
WaitResult FutexWait(const std::atomic<uint32_t>& futex, const int expected_state, const timespec* timeout) {
  const int64_t result = syscall(SYS_futex, &futex, FUTEX_WAIT, expected_state, timeout, nullptr, 0);
  const int error_number = errno;
  if (result == 0) {
    // Normal wake-up via FUTEX_WAKE
    SPDLOG_DEBUG("futex_wait: FUTEX_WAKE");
    return WaitResult::Success;
  }

  // All error cases come through here
  switch (error_number) {
    case EAGAIN:  // Value was already different when we tried to wait
      SPDLOG_DEBUG("futex_wait: EAGAIN");
      return WaitResult::Success;
    case EINTR:  // Interrupted by signal (like SIGINT)
      SPDLOG_DEBUG("futex_wait: EINTR (interrupted)");
      return WaitResult::Interrupted;
    case ETIMEDOUT:
      SPDLOG_WARN("FutexWait timed out");
      return WaitResult::Timeout;
    // LCOV_EXCL_START
    default:
      SPDLOG_ERROR("futex_wait error: {}", detail::FormatSystemError("futex_wait error", error_number));
      return WaitResult::Error;
      // LCOV_EXCL_STOP
  }
}

bool FutexWake(const std::atomic<uint32_t>& futex, const int count) {
  const int64_t result = syscall(SYS_futex, &futex, FUTEX_WAKE, count, nullptr, nullptr, 0);
  if (result == -1) {
    // LCOV_EXCL_START
    SPDLOG_ERROR("futex_wake error: {}", detail::FormatSystemError("futex_wake error", errno));
    return false;
    // LCOV_EXCL_STOP
  }
  return true;
}

}  // namespace dex::shared_memory::detail
