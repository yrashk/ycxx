// [utility.unreachable]: reaching __builtin_unreachable() is undefined, so a program that never
// reaches it runs normally; it is a void expression usable wherever control cannot continue.
#if !__has_builtin(__builtin_unreachable)
#error __builtin_unreachable is unavailable
#endif

static_assert(__is_same(decltype(__builtin_unreachable()), void));

[[noreturn]] inline void stop() { __builtin_unreachable(); }

int classify(int x) {
  switch (x & 1) {
    case 0: return 10;
    case 1: return 11;
  }
  __builtin_unreachable();
}

template<class T>
T positive(T x) {
  if (x > 0) return x;
  __builtin_unreachable();
}

int main(int argc, char**) {
  if (argc > 5) stop();
  int chosen = argc > 0 ? 3 : (__builtin_unreachable(), 4);
  if (argc <= 0) __builtin_unreachable();
  return classify(argc) == 11 && positive(argc) == 1 && chosen == 3 ? 0 : 1;
}
