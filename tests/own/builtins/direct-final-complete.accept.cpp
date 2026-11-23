// #1046 [meta.unary.prop], [class.pre].
union Storage { int value; };
template<class T> struct Slot { T value; };
using Packet = Slot<int>;
struct Sealed final {};

static_assert(!__is_final(const Storage));
static_assert(!__is_final(volatile Packet));
static_assert(__is_final(const volatile Sealed));
