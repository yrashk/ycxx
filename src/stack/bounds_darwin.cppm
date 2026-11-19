// This thread's stack on macOS: its highest address and its size, from pthreads.
module;
#include <pthread.h>
export module ycxx.stack:bounds;

import std;

export namespace ycxx::stack {

// The stack's lowest and highest addresses.
std::pair<std::uintptr_t, std::uintptr_t> bounds() noexcept {
  const pthread_t self = ::pthread_self();
  const auto high = reinterpret_cast<std::uintptr_t>(::pthread_get_stackaddr_np(self));
  return {high - ::pthread_get_stacksize_np(self), high};
}

}  // namespace ycxx::stack
