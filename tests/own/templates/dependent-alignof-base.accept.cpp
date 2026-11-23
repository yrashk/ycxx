// [expr.alignof], [temp.dep.constexpr], [temp.arg.nontype]
using Size = decltype(sizeof(0));

template<Size N> struct Value { static constexpr Size value = N; };
template<class T> struct Alignment : Value<alignof(T)> {};
template<class T> constexpr Size alignment_value = alignof(T);
template<class T> constexpr Size direct_alignment() { return alignof(T); }

struct alignas(16) Aligned { char byte; };
static_assert(Alignment<Aligned>::value == 16);
static_assert(direct_alignment<Aligned>() == 16);
static_assert(alignment_value<Aligned> == 16);
static_assert(alignof(Aligned[3]) == 16);
static_assert(Alignment<Aligned&>::value == 16);
static_assert(Alignment<Aligned[]>::value == 16);
