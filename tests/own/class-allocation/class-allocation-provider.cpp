// Replaces the usual single-object allocation functions to observe what new and delete
// expressions of class type pass them ([new.delete.single], [expr.new], [expr.delete]).
using Size = decltype(sizeof(0));

namespace {
constexpr int slots = 16;
alignas(16) unsigned char pool[slots][64];
Size requested[slots];
void* released[slots];
Size released_bytes[slots];
int allocations, releases, unsized;
}  // namespace

void* operator new(Size bytes) {
  if (allocations == slots || bytes > sizeof pool[0]) __builtin_trap();
  requested[allocations] = bytes;
  // Fill the storage, so a value-initialization that skips its zero-initialization shows.
  for (Size i = 0; i < bytes; ++i) pool[allocations][i] = 0xa5;
  return pool[allocations++];
}
void operator delete(void* address) noexcept {
  if (address) ++unsized;
}
void operator delete(void* address, Size bytes) noexcept {
  if (!address) return;
  if (releases == slots) __builtin_trap();
  released[releases] = address;
  released_bytes[releases++] = bytes;
}

int allocation_count() noexcept { return allocations; }
Size allocation_size(int i) noexcept { return requested[i]; }
void* allocation_address(int i) noexcept { return pool[i]; }
int release_count() noexcept { return releases; }
Size release_size(int i) noexcept { return released_bytes[i]; }
void* release_address(int i) noexcept { return released[i]; }
int unsized_releases() noexcept { return unsized; }
