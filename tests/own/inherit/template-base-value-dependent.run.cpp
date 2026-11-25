// A class template's base can be a template-id whose argument is a value-dependent expression
// ([temp.dep.constexpr]); the base is substituted when a specialization is instantiated
// ([temp.inst]), and qualified lookup finds the inherited member ([class.member.lookup]) (#308).
// EXIT: 0
using size_t = decltype(sizeof 0);
template<bool B> struct BC { static constexpr bool value = B; };
template<class T, T V> struct IC { static constexpr T value = V; };
template<bool B, class T, class U> struct cond { using type = T; };
template<class T, class U> struct cond<false, T, U> { using type = U; };
template<bool B, class T, class U> using cond_t = typename cond<B, T, U>::type;

struct Yes { static constexpr bool value = true; static constexpr int size = 4; };
struct No { static constexpr bool value = false; static constexpr int size = 6; };

// A primary template: a qualified static member, a negated cast of it, and an integer expression.
template<class T> struct Same : BC<T::value> {};
template<class T> struct Not : BC<!bool(T::value)> {};
template<class T> struct Twice : IC<int, T::size * 2 + 1> {};
template<class T, size_t N> struct Wide : IC<bool, (sizeof(T) > N)> {};

static_assert(Same<Yes>::value && !Same<No>::value);
static_assert(!Not<Yes>::value && Not<No>::value);
static_assert(Twice<Yes>::value == 9 && Twice<No>::value == 13);
static_assert(Wide<long long, 1>::value && !Wide<char, 1>::value);

// A partial specialization whose base selects a type by `||`, `!` and `sizeof...`, as the
// standard's conjunction and disjunction do ([meta.logical]).
template<class... B> struct conj : BC<true> {};
template<class B1, class... Bn>
struct conj<B1, Bn...> : cond_t<!bool(B1::value) || sizeof...(Bn) == 0, B1, conj<Bn...>> {};
template<class... B> struct disj : BC<false> {};
template<class B1, class... Bn>
struct disj<B1, Bn...> : cond_t<bool(B1::value) || sizeof...(Bn) == 0, B1, disj<Bn...>> {};

static_assert(conj<>::value && conj<Yes>::value && conj<Yes, Yes, Yes>::value);
static_assert(!conj<No>::value && !conj<Yes, No, Yes>::value);
static_assert(!disj<>::value && disj<No, No, Yes>::value && !disj<No, No>::value);
// The deciding trait is the base: conj<Yes, No> derives from No.
static_assert(__is_base_of(No, conj<Yes, No>) && __is_base_of(Yes, disj<No, Yes, No>));

bool through_base(const BC<false>& b) { return b.value; }

int main() {
  Not<Yes> n;
  const BC<false>& b = n;  // derived-to-base conversion
  return Same<Yes>::value && !Not<Yes>::value && Twice<No>::value == 13 && !through_base(n) && !b.value &&
                 conj<Yes, Yes>::value && !conj<Yes, No>::value && disj<No, Yes>::value
             ? 0
             : 1;
}
