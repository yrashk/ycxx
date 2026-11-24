// #1172: __int128 and unsigned __int128, the GNU extended integer types (docs/EXTENSIONS.md):
// their specifiers ([dcl.type.simple]), distinct types, size and alignment on each target's ABI,
// conversion rank ([conv.rank]), promotions ([conv.prom]), the usual arithmetic conversions
// ([expr.arith.conv]) and overload resolution ([over.ics.rank]).
// FLAGS: -Wno-pedantic
template <class T, class U> constexpr bool same = false;
template <class T> constexpr bool same<T, T> = true;

static_assert(sizeof(__int128) == 16 && alignof(__int128) == 16);
static_assert(sizeof(unsigned __int128) == 16 && alignof(unsigned __int128) == 16);
static_assert(same<signed __int128, __int128> && same<__int128 signed, __int128>);
static_assert(same<__int128 unsigned, unsigned __int128>);
static_assert(same<__int128_t, __int128> && same<__uint128_t, unsigned __int128>);
static_assert(!same<__int128, long long> && !same<unsigned __int128, unsigned long long>);
static_assert(!same<__int128, long> && !same<__int128, unsigned __int128>);
static_assert(same<const __int128, __int128 const>);

struct S {
  char c;
  __int128 i;
};
static_assert(sizeof(S) == 32 && alignof(S) == 16);

// The rank is above long long's, so neither type promotes.
__int128 i;
unsigned __int128 u;
long long ll;
unsigned long long ull;
int n;
unsigned un;
bool b;
char c;
static_assert(same<decltype(+i), __int128> && same<decltype(-u), unsigned __int128>);
static_assert(same<decltype(~i), __int128>);
static_assert(same<decltype(i + ull), __int128>);
static_assert(same<decltype(ll + u), unsigned __int128>);
static_assert(same<decltype(i + u), unsigned __int128>);
static_assert(same<decltype(n * i), __int128> && same<decltype(un - i), __int128>);
static_assert(same<decltype(i << n), __int128> && same<decltype(n << i), int>);
static_assert(same<decltype(b ? i : c), __int128>);
static_assert(same<decltype(i / 2.0), double> && same<decltype(u * 1.0f), float>);
static_assert(same<decltype(i < u), bool>);

template <class T> struct P;
template <> struct P<__int128> { static constexpr int v = 1; };
template <> struct P<unsigned __int128> { static constexpr int v = 2; };
template <> struct P<long long> { static constexpr int v = 3; };
static_assert(P<__int128>::v == 1 && P<unsigned __int128>::v == 2 && P<__int128_t>::v == 1);

// An exact match beats the integral conversions.
constexpr int f(__int128) { return 1; }
constexpr int f(unsigned __int128) { return 2; }
constexpr int f(long long) { return 3; }
static_assert(f(__int128(0)) == 1 && f(static_cast<unsigned __int128>(0)) == 2 && f(0ll) == 3);

template <class T> constexpr T id(T t) { return t; }
static_assert(same<decltype(id(i)), __int128> && same<decltype(id(u)), unsigned __int128>);

template <__int128 V> constexpr __int128 value = V;
static_assert(value<(__int128)1 << 100> == (__int128)1 << 100);
