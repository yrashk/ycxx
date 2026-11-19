// This thread's stack on Linux: its lowest address and its size, from pthreads.
module;
#include <pthread.h>
export module ycxx.stack:bounds;

import std;

export namespace ycxx::stack {

// The stack's lowest and highest addresses; nothing known (0, 0) if pthreads cannot say.
std::pair<std::uintptr_t, std::uintptr_t> bounds() noexcept {
  pthread_attr_t a;
  if (::pthread_getattr_np(::pthread_self(), &a) != 0) return {0, 0};
  void* low = nullptr;
  std::size_t size = 0;
  const int r = ::pthread_attr_getstack(&a, &low, &size);
  ::pthread_attr_destroy(&a);
  if (r != 0) return {0, 0};
  const auto l = reinterpret_cast<std::uintptr_t>(low);
  return {l, l + size};
}

}  // namespace ycxx::stack
