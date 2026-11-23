#include "try_or_null.hpp"

static_assert(!__ycxx::__detail::__cfg::exceptions);

extern "C" int work(int input);
extern "C" void mark_cleanup() noexcept;
extern "C" void* make_value();
extern "C" int foreign();

extern "C" int plain_frame(int input) {
  return work(input);
}

struct Guard {
  ~Guard() { mark_cleanup(); }
};

extern "C" int ordinary_cleanup(int input) {
  Guard guard{};
  return work(input);
}

struct Ordinary {
  int value = foreign();
};

extern "C" int ordinary_member_init() {
  return Ordinary{}.value;
}

void* factory() {
  return make_value();
}

extern "C" void* pinned_try_or_null(bool heapless) {
  return __ycxx::__detail::__try_or_null(heapless, &factory);
}
