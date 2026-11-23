// #209 [temp.names], [temp.concept], [temp.dep.constexpr], [temp.arg.nontype].
namespace Check {
template<class T, class... A>
concept Fits = (sizeof...(A) == 0) || (sizeof(T) == 1 && sizeof...(A) == 1);

template<class T>
constexpr bool wide = sizeof(T) > 1;
}

template<bool Result>
struct Bool {};

static_assert(Check::Fits<int>);
static_assert(__is_same(Bool<Check::Fits<int>>, Bool<true>));
static_assert(__is_same(Bool<Check::Fits<char[2], int>>, Bool<false>));
static_assert(__is_same(Bool<Check::wide<char[2]>>, Bool<true>));

template<class T, class... A>
struct Record : Bool<Check::Fits<T, A...>> {};

static_assert(__is_base_of(Bool<true>, Record<char, int>));
static_assert(__is_base_of(Bool<false>, Record<char[2], int>));
static_assert(__is_base_of(Bool<false>, Record<char, int, long>));
static_assert(__is_base_of(Bool<true>, Record<char[2]>));

template<class T> constexpr int pick() { return 1; }
template<class T> requires Check::Fits<T, int>
constexpr int pick() { return 2; }

static_assert(pick<char>() == 2);
static_assert(pick<char[2]>() == 1);
