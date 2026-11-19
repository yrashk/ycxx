// ζ t8 (#88), from the step's review: `__make_integer_seq<…>` names a class, with members, for N of
// another integral type than T, and with an alias template as Seq (docs/EXTENSIONS.md).
// XFAIL: gcc GCC has no __make_integer_seq
// EXIT: 0
template<class T, T... Is> struct iseq { static constexpr int n = sizeof...(Is); static constexpr long long sum = (0LL + ... + (long long)Is); };
enum E : int { a, b };
template<class T, T... Is> using alias_seq = iseq<T, Is...>;
static_assert(__make_integer_seq<iseq, long, 3u>::n == 3);
static_assert(__make_integer_seq<iseq, unsigned char, 4>::sum == 6);
static_assert(__make_integer_seq<iseq, short, (char)2>::n == 2);
static_assert(__is_same(__make_integer_seq<iseq, long, 2>, iseq<long, 0, 1>));
static_assert(__is_same(__make_integer_seq<alias_seq, int, 2>, iseq<int, 0, 1>));
static_assert(__make_integer_seq<iseq, unsigned, 1000>::n == 1000);
int main() { return 0; }
