using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }
namespace fixture { struct Token { int key; }; }
using Tag = fixture::Token;
using Alignment = std::align_val_t;

alignas(unsigned long) static unsigned char first_storage[32];
alignas(unsigned long) static unsigned char second_storage[32];
alignas(unsigned long) static unsigned char ordinary_storage[32];
static const Tag* first_tag;
static const Tag* second_tag;
static int stage;
static int failure;

static void observe(bool correct, int code) {
  if (!correct && failure == 0) failure = code;
}

void* operator new(Count count, const Tag& tag) noexcept {
  observe(stage == 0 && count == 17 && tag.key == 13, 11);
  first_tag = &tag;
  stage = 1;
  return first_storage;
}

void operator delete(void* address, const Tag& tag) {
  observe(stage == 1 && address == first_storage &&
          &tag == first_tag && tag.key == 13, 12);
  stage = 2;
}

void* operator new(Count count, Alignment alignment, const Tag& tag) noexcept {
  observe(stage == 2 && count == 23 && tag.key == 29 &&
          static_cast<Count>(alignment) == alignof(unsigned long), 21);
  second_tag = &tag;
  stage = 3;
  return second_storage;
}

void operator delete(void* address, Alignment alignment, const Tag& tag) noexcept {
  observe(stage == 3 && address == second_storage && &tag == second_tag &&
          tag.key == 29 && static_cast<Count>(alignment) == alignof(unsigned long), 22);
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

extern "C" int inspect_ordinary(int* address) {
  observe(stage == 5 && static_cast<void*>(address) == ordinary_storage, 34);
  return *address;
}

extern "C" int tagged_probe_status() {
  if (failure) return failure;
  return stage == 6 ? 0 : 35;
}
