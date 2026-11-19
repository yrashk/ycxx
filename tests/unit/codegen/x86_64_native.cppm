module;
#include <sys/mman.h>
export module ycxx.unit.x86_64_native;

import std;

export namespace ycxx::unit {

class Executable {
 public:
  explicit Executable(std::span<const std::uint8_t> bytes) : size_(bytes.size()) {
    void* mapped = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return;
    std::memcpy(mapped, bytes.data(), size_);
    if (mprotect(mapped, size_, PROT_READ | PROT_EXEC) != 0) {
      munmap(mapped, size_);
      return;
    }
    memory_ = static_cast<std::uint8_t*>(mapped);
  }

  Executable(const Executable&) = delete;
  Executable& operator=(const Executable&) = delete;
  ~Executable() { if (memory_) munmap(memory_, size_); }

  explicit operator bool() const { return memory_ != nullptr; }

  template <class Function>
  Function* function() const { return reinterpret_cast<Function*>(memory_); }

 private:
  std::size_t size_;
  std::uint8_t* memory_ = nullptr;
};

}  // namespace ycxx::unit
