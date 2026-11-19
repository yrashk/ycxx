// The usable size of a block from `malloc`, for an unsized delete (#274). CMake picks this file
// or usable_linux.cppm.
module;
#include <malloc/malloc.h>
export module ycxx.memory:usable;

import std;

export namespace ycxx::memory {
std::size_t usable_size(void* p) noexcept { return ::malloc_size(p); }
}  // namespace ycxx::memory
