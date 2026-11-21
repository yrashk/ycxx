constexpr int select(int) noexcept { return 10; }
constexpr int select(long) { return 20; }
constexpr int (*exact)(int) noexcept = &select;
constexpr int (*relaxed)(int) = select;
constexpr int (&other)(long) = select;
static_assert(exact(1) + relaxed(1) == 20);
int call_reference() { return other(1); }
