// A dependent predicate selects the ordinary bool specialization ([temp.alias], [temp.type], #209).
// EXIT: 0
template<bool B> struct Constant { static constexpr bool value = B; };
template<bool B> using Bool = Constant<B>;
template<class T, class U> using IsSame = Bool<__is_same(T, U)>;

static_assert(IsSame<int, int>::value);
static_assert(!IsSame<int, long>::value);
static_assert(__is_same(IsSame<int, int>, Bool<true>));
static_assert(__is_same(IsSame<int, long>, Bool<false>));
static_assert(!__is_same(IsSame<int, int>, IsSame<int, long>));

int main() {
  return IsSame<int, int>::value && !IsSame<int, long>::value ? 0 : 1;
}
