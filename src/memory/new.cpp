// The program's replacements of the global allocation functions ([replacement.functions], #274):
// `malloc` and `free`, as libycxx's own defaults do, with every size counted by ycxx.memory.
// libycxx's defaults are weak, and its allocation table sends every image of the process to these.
// Linked into `ycxx` and the unit tests that check memory, as an object library: a static
// library's member would lose to libycxx's defaults depending on link order.
import std;
import ycxx.memory;

namespace {

namespace memory = ycxx::memory;

// The alignment `malloc` gives every block: 16 on Darwin's and glibc's 64-bit targets.
constexpr std::size_t malloc_align = 16;

void* raw(std::size_t n, std::size_t align) noexcept {
  if (n == 0) n = 1;
  if (align <= malloc_align) return std::malloc(n);
  if (n > std::numeric_limits<std::size_t>::max() - align) return nullptr;  // an overflowed array size
  return std::aligned_alloc(align, (n + align - 1) / align * align);
}

void* allocate(std::size_t n, std::size_t align) {
  for (;;) {
    if (void* p = raw(n, align)) {
      memory::on_allocate(n);
      return p;
    }
    const std::new_handler h = std::get_new_handler();
    if (!h) throw std::bad_alloc();
    h();
  }
}

void* allocate_nothrow(std::size_t n, std::size_t align) noexcept {
  try {
    return allocate(n, align);
  } catch (...) {
    return nullptr;
  }
}

void release(void* p, std::size_t n) noexcept {
  if (!p) return;
  memory::on_free(n);
  std::free(p);
}

void release_unsized(void* p) noexcept {
  if (!p) return;
  if (memory::global.counting.load(std::memory_order_relaxed)) memory::on_free(memory::usable_size(p));
  std::free(p);
}

constexpr std::size_t al(std::align_val_t a) { return static_cast<std::size_t>(a); }

const bool installed = (memory::install(), true);

}  // namespace

void* operator new(std::size_t n) { return allocate(n, 0); }
void* operator new[](std::size_t n) { return allocate(n, 0); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate(n, al(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate(n, al(a)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return allocate_nothrow(n, 0); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return allocate_nothrow(n, 0); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate_nothrow(n, al(a)); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocate_nothrow(n, al(a)); }

void operator delete(void* p) noexcept { release_unsized(p); }
void operator delete[](void* p) noexcept { release_unsized(p); }
void operator delete(void* p, std::size_t n) noexcept { release(p, n); }
void operator delete[](void* p, std::size_t n) noexcept { release(p, n); }
void operator delete(void* p, std::align_val_t) noexcept { release_unsized(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release_unsized(p); }
void operator delete(void* p, std::size_t n, std::align_val_t) noexcept { release(p, n); }
void operator delete[](void* p, std::size_t n, std::align_val_t) noexcept { release(p, n); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release_unsized(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release_unsized(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release_unsized(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release_unsized(p); }
