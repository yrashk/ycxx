// A dependent alias selects its base before qualified member lookup ([temp.inst], #308).
template<bool B> struct Constant { static constexpr bool value = B; };
template<bool B> using Bool = Constant<B>;
template<class T, class U> struct IsSame : Bool<__is_same(T, U)> {};

static_assert(IsSame<int, int>::value);
static_assert(!IsSame<int, long>::value);

using Integer = int;
using Equal = IsSame<Integer, int>;
using Unequal = IsSame<int, long>;
static_assert(Equal::value && !Unequal::value);
static_assert(__is_same(Equal, IsSame<int, int>));
static_assert(__is_same(Unequal, IsSame<Integer, long>));
static_assert(!__is_same(Equal, Unequal));
