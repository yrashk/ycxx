using Plain = void(int);
using Safe = void(int) noexcept;
void run(int) noexcept(true);
void other(int) noexcept(false);
static_assert(__is_same(decltype(run), Safe));
static_assert(__is_same(decltype(&other), Plain*));
static_assert(__is_same(decltype((run)), Safe&));
static_assert(!__is_same(Plain*, Safe*));
static_assert(!__is_same(Plain&, Safe&));
