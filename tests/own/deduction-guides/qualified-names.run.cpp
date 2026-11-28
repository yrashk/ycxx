// #316: a namespace's class template named by a qualified name deduces its arguments too ([dcl.type.class.deduct]/1, [expr.type.conv]/1)
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

namespace n {
template <class T, class U> struct pair {
    T first;
    U second;
    pair(T a, U b) : first(a), second(b) {}
};
template <class T, class U> pair(T, U) -> pair<T, U>;
namespace inner {
template <class T> struct box {
    T v;
};
template <class T> box(T) -> box<T>;
}  // namespace inner
}  // namespace n

n::pair global(1, 'b');
static_assert(same<decltype(global), n::pair<int, char>>::value);

int main() {
    n::pair p(2, 3.0);
    static_assert(same<decltype(p), n::pair<int, double>>::value);
    n::inner::box b{4L};
    static_assert(same<decltype(b), n::inner::box<long>>::value);
    auto q = n::pair(5u, 6);
    static_assert(same<decltype(q), n::pair<unsigned, int>>::value);
    auto c = ::n::inner::box{7};
    return global.first + p.first + int(b.v) + int(q.first) + c.v - 19;
}
