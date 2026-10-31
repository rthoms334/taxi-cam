#pragma once
#include <cstddef>
#include <memory>
#include <new>

namespace taxi_camera {
// A 32,768-character wide path is 64 KiB. Bridge code runs on threads whose
// stack the host chose, inside hooked graphics calls and during loader-driven
// startup, so such buffers live on the heap. Allocation is nothrow: callers
// are noexcept and must refuse the operation when `!buffer`.
class WidePathBuffer {
 public:
  static constexpr std::size_t capacity = 32768;
  WidePathBuffer() noexcept : data_(new (std::nothrow) wchar_t[capacity]()) {}
  explicit operator bool() const noexcept { return data_ != nullptr; }
  wchar_t* get() noexcept { return data_.get(); }
  const wchar_t* get() const noexcept { return data_.get(); }
  static constexpr std::size_t size() noexcept { return capacity; }

 private:
  std::unique_ptr<wchar_t[]> data_;
};
}  // namespace taxi_camera
