template<bool Value>
struct AssignmentResult {
    static constexpr bool value = Value;
};

template<class T>
using Operand = T;

template<class T, class U>
struct Assignable : AssignmentResult<__is_assignable(Operand<T>, Operand<U>)> {};

template<class T, class U>
struct NothrowAssignable : AssignmentResult<__is_nothrow_assignable(Operand<T>, Operand<U>)> {};

template<class T, class U>
struct TriviallyAssignable : AssignmentResult<__is_trivially_assignable(Operand<T>, Operand<U>)> {};

using Mutable = volatile int&;
using Immutable = const int&;
using Xvalue = int&&;
using Source = double;

static_assert(Assignable<Mutable, Source>::value);
static_assert(NothrowAssignable<Mutable, Source>::value);
static_assert(TriviallyAssignable<Mutable, Source>::value);

static_assert(!Assignable<Immutable, Source>::value);
static_assert(!NothrowAssignable<Immutable, Source>::value);
static_assert(!TriviallyAssignable<Immutable, Source>::value);

static_assert(!Assignable<Xvalue, Source>::value);
static_assert(!NothrowAssignable<Xvalue, Source>::value);
static_assert(!TriviallyAssignable<Xvalue, Source>::value);
