// [utility.unreachable], [dcl.attr.noreturn]: __builtin_unreachable() ends a path, so a noreturn
// function or a value-returning function may end with it instead of a return.
[[noreturn]] void halt() { __builtin_unreachable(); }

[[noreturn]] inline void halt_inline() {
  if (true) __builtin_unreachable();
  __builtin_unreachable();
}

int sign(int x) {
  if (x < 0) return -1;
  if (x > 0) return 1;
  if (x == 0) return 0;
  __builtin_unreachable();
}

int after_halt(int x) {
  if (x) return x;
  halt();
}

auto deduced(bool b) {
  if (b) return 1;
  __builtin_unreachable();
}
static_assert(__is_same(decltype(deduced(true)), int));

struct Visitor {
  int value;
  int get() const {
    if (value >= 0) return value;
    __builtin_unreachable();
  }
};

constexpr int constant_get() {
  Visitor v{5};
  return v.value == 5 ? 5 : (__builtin_unreachable(), 0);
}
static_assert(constant_get() == 5);
