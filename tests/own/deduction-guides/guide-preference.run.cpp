// #316: a deduction-guide wins over an equally good implicit guide, and a non-template guide over templates ([over.match.best.general]/2.4, /2.10)
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

template <class T> struct S {
    T v;
    S(T x) : v(x) {}
};
template <class T> S(T) -> S<const T>;
S(short) -> S<long>;

int main() {
    S a(1);
    static_assert(same<decltype(a), S<const int>>::value);
    short n = 2;
    S b(n);
    static_assert(same<decltype(b), S<long>>::value);
    return a.v + b.v - 3;
}
