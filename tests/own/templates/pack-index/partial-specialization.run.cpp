// #297: a partial specialization whose argument is a pack-index-specifier of a pack deduced from
// its other arguments ([temp.spec.partial.match], [temp.deduct.type]/5).
// XFAIL: clang Clang 23 never matches the specialization, though T is deduced from `T...`
// EXIT: 0
template<class X, class... T> struct P { static constexpr int v = 0; };
template<class... T> struct P<T...[0]*, T...> { static constexpr int v = 1; };
static_assert(P<int*, int, char>::v == 1);
static_assert(P<char*, int>::v == 0);
int main() { return 0; }
