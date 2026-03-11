#pragma once

#include <memory>
#include <type_traits>

namespace dex::shared_memory::detail {

/// A minimal implementation of observer_ptr (the "world's dumbest smart pointer")
/// to replace std::experimental::observer_ptr which is not available on all platforms.
template <typename T>
class observer_ptr {
 public:
  using element_type = T;

  constexpr observer_ptr() noexcept : ptr_(nullptr) {}
  constexpr observer_ptr(std::nullptr_t) noexcept : ptr_(nullptr) {}
  constexpr explicit observer_ptr(element_type* p) noexcept : ptr_(p) {}

  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  constexpr observer_ptr(observer_ptr<U> other) noexcept : ptr_(other.get()) {}

  constexpr element_type* get() const noexcept { return ptr_; }
  constexpr element_type* operator->() const noexcept { return ptr_; }
  constexpr element_type& operator*() const noexcept { return *ptr_; }
  constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

  constexpr element_type* release() noexcept {
    element_type* p = ptr_;
    ptr_ = nullptr;
    return p;
  }

  constexpr void reset(element_type* p = nullptr) noexcept { ptr_ = p; }

  friend constexpr bool operator==(observer_ptr p, std::nullptr_t) noexcept { return p.get() == nullptr; }
  friend constexpr bool operator==(std::nullptr_t, observer_ptr p) noexcept { return p.get() == nullptr; }
  friend constexpr bool operator!=(observer_ptr p, std::nullptr_t) noexcept { return p.get() != nullptr; }
  friend constexpr bool operator!=(std::nullptr_t, observer_ptr p) noexcept { return p.get() != nullptr; }

  friend constexpr bool operator==(observer_ptr lhs, observer_ptr rhs) noexcept { return lhs.get() == rhs.get(); }
  friend constexpr bool operator!=(observer_ptr lhs, observer_ptr rhs) noexcept { return lhs.get() != rhs.get(); }

 private:
  element_type* ptr_;
};

}  // namespace dex::shared_memory::detail
