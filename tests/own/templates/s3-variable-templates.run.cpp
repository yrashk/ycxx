// ζ s3 (#88): variable templates: a constexpr initializer evaluated for each specialization,
// default arguments, one variable template's initializer naming another, a specialization's
// address, and a namespace's variable template named with a qualified template-id
// ([temp.pre], [temp.inst]/7, [temp.arg], [namespace.qual]).
// EXIT: 0
template<class T>
inline constexpr bool is_int = false;
template<class T>
constexpr T zero = T(0);
template<class T, int N = 3>
constexpr T times = T(N) * T(2);
template<class T>
constexpr bool zero_or_int = is_int<T> || zero<T> == 0;

namespace num {
  template<class T>
  constexpr T one = T(1);
}

static_assert(!is_int<long>);
static_assert(zero<int> == 0);
static_assert(times<int> == 6);
static_assert(times<long, 5> == 10L);
static_assert(zero_or_int<char>);
static_assert(num::one<int> + num::one<long> == 2);

long twice_one() { return num::one<long> + *&num::one<long>; }

int main() {
  if (times<int, 4> != 8) return 1;
  const int* p = &zero<int>;
  if (*p != 0) return 2;
  if (twice_one() != 2) return 3;
  return zero_or_int<long> ? 0 : 4;
}
