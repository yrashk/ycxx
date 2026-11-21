template<bool Value>
struct ConversionResult {
    static constexpr bool value = Value;
};

template<class T>
using Operand = T;

template<class From, class To>
struct Convertible : ConversionResult<__is_convertible(Operand<From>, Operand<To>)> {};

template<class From, class To>
struct NothrowConvertible : ConversionResult<__is_nothrow_convertible(Operand<From>, Operand<To>)> {};

using Source = int&;
using Related = int&&;
using Unrelated = long&&;

static_assert(Convertible<Source, Unrelated>::value);
static_assert(NothrowConvertible<Source, Unrelated>::value);
static_assert(!Convertible<Source, Related>::value);
static_assert(!NothrowConvertible<Source, Related>::value);
