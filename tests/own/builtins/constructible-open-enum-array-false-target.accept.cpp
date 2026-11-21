enum Complete { initial };
using Open = const Complete[][2];
static_assert(!__is_constructible(const volatile void, Open));
