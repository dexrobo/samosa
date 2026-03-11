#include "dex/infrastructure/shared_memory/futex.h"

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#ifdef __APPLE__
#include <errno.h>
#include <sys/time.h>

extern "C" {
// Private Darwin primitives for futex-like synchronization
int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout);
int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
}

#define UL_COMPARE_AND_WAIT 1
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL 0x00000100
#endif

#include <unistd.h>

#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory_private.h"

namespace dex::shared_memory::detail {

////////////////////////////////////////////////////////////////////////////////
/// Futex helper functions for conditional synchronization.
////////////////////////////////////////////////////////////////////////////////
// Wait on the futex at addr until *addr != expected.
WaitResult FutexWait(const std::atomic<uint32_t>& futex, const int expected_state, const timespec* timeout) {
#ifdef __linux__
  const int64_t result = syscall(SYS_futex, &futex, FUTEX_WAIT, expected_state, timeout, nullptr, 0);
  const int error_number = errno;
#elif defined(__APPLE__)
  uint32_t timeout_us = 0;
  if (timeout != nullptr) {
    timeout_us = static_cast<uint32_t>(timeout->tv_sec * 1000000 + timeout->tv_nsec / 1000);
    if (timeout_us == 0 && timeout->tv_nsec > 0) {
      timeout_us = 1;  // Minimum 1us if timeout is non-zero
    }
  }

  // __ulock_wait returns 0 on success, -1 on error
  // Use UL_COMPARE_AND_WAIT_SHARED for cross-process support
  int result = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED, const_cast<std::atomic<uint32_t>*>(&futex),
                            static_cast<uint64_t>(expected_state), timeout_us);
  const int error_number = (result < 0) ? errno : 0;
#endif

  if (result >= 0) {
    // Normal wake-up via FUTEX_WAKE
    SPDLOG_DEBUG("futex_wait: WOKEN");
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
    default:
      SPDLOG_ERROR("futex_wait error: {}", detail::FormatSystemError("futex_wait error", error_number));
      return WaitResult::Error;
  }
}

bool FutexWake(const std::atomic<uint32_t>& futex, const int count) {
#ifdef __linux__
  const int64_t result = syscall(SYS_futex, &futex, FUTEX_WAKE, count, nullptr, nullptr, 0);
  const int error_number = errno;
#elif defined(__APPLE__)
  uint32_t operation = UL_COMPARE_AND_WAIT_SHARED;
  if (count > 1) {
    operation |= ULF_WAKE_ALL;
  }
  int result = __ulock_wake(operation, const_cast<std::atomic<uint32_t>*>(&futex), 0);
  if (result < 0 && errno == ENOENT) {
    // ENOENT on MacOS means there are no waiters, which is not an error
    return true;
  }
  const int error_number = (result < 0) ? errno : 0;
#endif

  if (result < 0) {
    SPDLOG_ERROR("futex_wake error: {}", detail::FormatSystemError("futex_wake error", error_number));
    return false;
  }
  return true;
}

}  // namespace dex::shared_memory::detail
