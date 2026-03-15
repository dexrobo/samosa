#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_FUTEX_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_FUTEX_H

#include <atomic>
#include <experimental/memory>
#include <memory>

#include "gsl/pointers"

namespace dex::shared_memory::detail {

using std::experimental::observer_ptr;

/// Result of a futex wait operation
enum class WaitResult {
  Success,      /// Normal wake-up or value mismatch (EAGAIN)
  Timeout,      /// Wait timed out (ETIMEDOUT)
  Interrupted,  /// Interrupted by signal (EINTR)
  Error,        /// Catch-all: Fatal system error (EFAULT, etc.)
};

// Futex helper functions (Linux-specific) for conditional synchronization.
// Wait on the futex at addr until *addr != expected.
[[nodiscard]] WaitResult FutexWait(const std::atomic<uint32_t>& futex, int expected, const timespec* timeout = nullptr);
[[nodiscard]] bool FutexWake(const std::atomic<uint32_t>& futex, int count);

// Define interface for futex operations
class Futex {
 public:
  Futex() = default;
  virtual ~Futex() = default;
  Futex(const Futex&) = delete;
  Futex& operator=(const Futex&) = delete;
  Futex(Futex&&) = delete;
  Futex& operator=(Futex&&) = delete;

  [[nodiscard]] virtual WaitResult Wait(const std::atomic<uint32_t>& futex, int expected,
                                        const timespec* timeout = nullptr) const = 0;

  [[nodiscard]] virtual bool Wake(const std::atomic<uint32_t>& futex, int count) const = 0;
};

// Default implementation using real futex operations
class DefaultFutex : public Futex {
 public:
  [[nodiscard]] WaitResult Wait(const std::atomic<uint32_t>& futex, int expected,
                                const timespec* timeout = nullptr) const override {
    return FutexWait(futex, expected, timeout);
  }

  [[nodiscard]] bool Wake(const std::atomic<uint32_t>& futex, int count) const override {
    return FutexWake(futex, count);
  }
};

// Manages the global futex instance
class FutexManager {
 public:
  static observer_ptr<const Futex> Get() {
    static const FutexManager& instance = GetInstance();  // Use GetInstance() for consistency
    return instance.futex_;
  }

  // For testing only
  static void Override(const Futex& new_futex) {
    static FutexManager& instance = GetInstance();
    instance.previous_futex_.reset(instance.futex_.release());
    instance.futex_.reset(&new_futex);  // Just borrowing
  }

  static void Restore() {
    static FutexManager& instance = GetInstance();
    instance.futex_.reset(instance.previous_futex_.release());
    instance.previous_futex_.reset();
  }

  ~FutexManager() = default;
  FutexManager(const FutexManager&) = delete;
  FutexManager& operator=(const FutexManager&) = delete;
  FutexManager(FutexManager&&) = delete;
  FutexManager& operator=(FutexManager&&) = delete;

 private:
  static FutexManager& GetInstance() {
    static FutexManager instance;
    return instance;
  }

  FutexManager() : owned_default_futex_(std::make_unique<DefaultFutex>()), futex_(owned_default_futex_.get()) {}

  std::unique_ptr<Futex> owned_default_futex_;         // Owns the default futex
  observer_ptr<const Futex> futex_;                    // Points to either owned_default_futex_ or borrowed futex
  observer_ptr<const Futex> previous_futex_{nullptr};  // Borrows previous futex
};

inline observer_ptr<const Futex> GetDefaultFutex() { return FutexManager::Get(); }

// Helper to set/reset futex operations
class ScopedFutex {
 public:
  explicit ScopedFutex(const Futex& futex) { FutexManager::Override(futex); }

  ~ScopedFutex() { FutexManager::Restore(); }

  ScopedFutex(const ScopedFutex&) = delete;
  ScopedFutex& operator=(const ScopedFutex&) = delete;
  ScopedFutex(ScopedFutex&&) = delete;
  ScopedFutex& operator=(ScopedFutex&&) = delete;
};

}  // namespace dex::shared_memory::detail
#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_FUTEX_H

