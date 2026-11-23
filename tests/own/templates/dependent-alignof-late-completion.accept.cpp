// [expr.alignof], [temp.point], [temp.inst]
using Size = decltype(sizeof(0));
struct Pending;
template<Size N> struct Value { static constexpr Size value = N; };
template<class T> struct Alignment : Value<alignof(T)> {};
template<class T> constexpr Size stored_alignment = alignof(T);

Alignment<Pending>* declaration;
struct alignas(16) Pending { char byte; };

static_assert(Alignment<Pending>::value == 16);
static_assert(stored_alignment<Pending> == 16);
