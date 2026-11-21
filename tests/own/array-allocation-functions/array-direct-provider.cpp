using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;

alignas(long double) static unsigned char blocks[6][64];
static unsigned stage;
static int failure;

static void check(bool valid, int code) {
  if (!valid && failure == 0) failure = code;
}

static void* allocate(unsigned slot, Count size, Count expected_size,
                      Count align, Count expected_align) {
  check(stage == 2 * slot && size == expected_size && align == expected_align, 10 + slot);
  ++stage;
  return blocks[slot];
}

static void release(unsigned slot, void* address, Count size, Count expected_size,
                    Count align, Count expected_align) {
  check(stage == 2 * slot + 1 && address == blocks[slot] &&
        size == expected_size && align == expected_align, 20 + slot);
  ++stage;
}

void* operator new[](Count size) {
  if (stage == 0) return allocate(0, size, 17, 0, 0);
  return allocate(2, size, 29, 0, 0);
}
void* operator new[](Count size, Alignment align) {
  if (stage == 2) return allocate(1, size, 23, static_cast<Count>(align), alignof(unsigned long));
  return allocate(3, size, 31, static_cast<Count>(align), alignof(int));
}
void operator delete[](void* address) noexcept { release(0, address, 0, 0, 0, 0); }
void operator delete[](void* address, Alignment align) noexcept {
  release(1, address, 0, 0, static_cast<Count>(align), alignof(unsigned long));
}
void operator delete[](void* address, Count size) noexcept { release(2, address, size, 29, 0, 0); }
void operator delete[](void* address, Count size, Alignment align) noexcept {
  release(3, address, size, 31, static_cast<Count>(align), alignof(int));
}

void* operator new(Count size) { return allocate(4, size, 37, 0, 0); }
void* operator new(Count size, Alignment align) {
  return allocate(5, size, 41, static_cast<Count>(align), alignof(int));
}
void operator delete(void* address, Count size) noexcept { release(4, address, size, 37, 0, 0); }
void operator delete(void* address, Count size, Alignment align) noexcept {
  release(5, address, size, 41, static_cast<Count>(align), alignof(int));
}
void operator delete(void* address) noexcept { if (address) check(false, 90); }
void operator delete(void* address, Alignment) noexcept { if (address) check(false, 91); }

extern "C" int array_direct_status() {
  if (failure) return failure;
  return stage == 12 ? 0 : 99;
}
