// The replacement allocation functions for builtin-operator-new.run.cpp, compiled by the oracle.
using Count = decltype(sizeof(0));
namespace std { enum class align_val_t : Count {}; }

enum Kind { plain_new = 1, aligned_new, plain_delete, sized_delete, aligned_delete, sized_aligned_delete };
struct Event { Kind kind; Count size; Count alignment; int slot; };
static constexpr Event expected[] = {
    {plain_new, 4, 0, 0},     {aligned_new, 16, 32, 1}, {plain_delete, 0, 0, 0},
    {aligned_delete, 0, 32, 1}, {aligned_new, 3 * sizeof(long), alignof(long), 2},
    {sized_aligned_delete, 3 * sizeof(long), alignof(long), 2}, {plain_new, 8, 0, 3}, {sized_delete, 8, 0, 3},
};
constexpr unsigned events = sizeof expected / sizeof expected[0];

alignas(32) static unsigned char storage[events][32];
static unsigned step;
static int failure;

static void record(Kind kind, Count size, Count alignment, void* address) {
  const unsigned at = step++;
  if (failure) return;
  if (at >= events) { failure = 99; return; }
  const Event& e = expected[at];
  if (e.kind != kind || e.size != size || e.alignment != alignment || address != storage[e.slot]) failure = 10 + int(at);
}
static void* next(Kind kind, Count size, Count alignment) {
  void* address = step < events ? storage[expected[step].slot] : nullptr;
  record(kind, size, alignment, address);
  return address;
}

void* operator new(Count size) { return next(plain_new, size, 0); }
void* operator new(Count size, std::align_val_t a) { return next(aligned_new, size, Count(a)); }
void operator delete(void* p) noexcept { record(plain_delete, 0, 0, p); }
void operator delete(void* p, Count size) noexcept { record(sized_delete, size, 0, p); }
void operator delete(void* p, std::align_val_t a) noexcept { record(aligned_delete, 0, Count(a), p); }
void operator delete(void* p, Count size, std::align_val_t a) noexcept { record(sized_aligned_delete, size, Count(a), p); }

extern "C" int allocation_log() { return failure ? failure : step == events ? 0 : 98; }
