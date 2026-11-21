template<class T>
using Operand = T;

template<class T, class U>
constexpr bool assignable = __is_assignable(Operand<T>, Operand<U>);

template<class T, class U>
constexpr bool nothrow_assignable = __is_nothrow_assignable(Operand<T>, Operand<U>);

template<class T, class U>
constexpr bool trivially_assignable = __is_trivially_assignable(Operand<T>, Operand<U>);

using Mutable = bool&;
using Immutable = const bool&;
using Value = bool;
using Source = const double&;

static_assert(assignable<Mutable, Source>);
static_assert(nothrow_assignable<Mutable, Source>);
static_assert(trivially_assignable<Mutable, Source>);

static_assert(!assignable<Immutable, Source>);
static_assert(!nothrow_assignable<Immutable, Source>);
static_assert(!trivially_assignable<Immutable, Source>);

static_assert(!assignable<Value, Source>);
static_assert(!nothrow_assignable<Value, Source>);
static_assert(!trivially_assignable<Value, Source>);
