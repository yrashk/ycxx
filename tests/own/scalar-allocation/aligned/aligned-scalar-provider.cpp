using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
using Alignment = std::align_val_t;

alignas(unsigned long) static unsigned char first_storage[32];
alignas(unsigned long) static unsigned char second_storage[32];
alignas(unsigned long) static unsigned char ordinary_storage[32];
static unsigned stage;
static int failure;

static void observe(bool valid, int code) {
  if (!valid && failure == 0) failure = code;
}

void* operator new(Count count, Alignment alignment) {
  if (stage == 0) {
    observe(count == 17 && static_cast<Count>(alignment) == alignof(unsigned long), 11);
    stage = 1;
    return first_storage;
  }
  observe(stage == 2 && count == 29 && static_cast<Count>(alignment) == alignof(int), 12);
  stage = 3;
  return second_storage;
}

void operator delete(void* address, Alignment alignment) noexcept {
  observe(stage == 1 && address == first_storage &&
          static_cast<Count>(alignment) == alignof(unsigned long), 21);
  stage = 2;
}

void operator delete(void* address, Count count, Alignment alignment) noexcept {
  observe(stage == 3 && address == second_storage &&
          count == 29 && static_cast<Count>(alignment) == alignof(int), 22);
  stage = 4;
}

void* operator new(Count count) {
  observe(stage == 4 && count == sizeof(int), 31);
  stage = 5;
  return ordinary_storage;
}

void operator delete(void* address) noexcept {
  if (address) observe(false, 32);
}

void operator delete(void* address, Count count) noexcept {
  observe(stage == 5 && address == ordinary_storage && count == sizeof(int), 33);
  stage = 6;
}

extern "C" int check_ordinary_value(int* address) {
  observe(stage == 5 && static_cast<void*>(address) == ordinary_storage, 34);
  return *address;
}

extern "C" int aligned_scalar_status() {
  if (failure) return failure;
  return stage == 6 ? 0 : 35;
}
