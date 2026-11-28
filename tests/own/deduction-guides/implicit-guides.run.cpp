// #316: the guides the constructors imply, the copy deduction candidate, and each form of initialization ([over.match.class.deduct]/1, [dcl.type.class.deduct])
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };

template <class T> struct Box {
    T v;
    Box(T x) : v(x) {}
};

template <class T, class U> struct Pair {
    T first;
    U second;
    Pair(T a, U b) : first(a), second(b) {}
};

template <class T> struct Holder {
    T v;
    template <class U> Holder(T x, U) : v(x) {}
};

template <class T = long> struct Defaulted {
    T v;
    Defaulted() : v(9) {}
};

Box ns(1.5);
static_assert(same<decltype(ns), Box<double>>::value);

int main() {
    Box direct(3);
    static_assert(same<decltype(direct), Box<int>>::value);
    Box list{'a'};
    static_assert(same<decltype(list), Box<char>>::value);
    Box copied = 4L;
    static_assert(same<decltype(copied), Box<long>>::value);
    // The copy deduction candidate: a Box<int>, not a Box<Box<int>>.
    Box again(direct);
    static_assert(same<decltype(again), Box<int>>::value);
    Box same_list{direct};
    static_assert(same<decltype(same_list), Box<int>>::value);
    const Box constant(7);
    static_assert(same<decltype(constant), const Box<int>>::value);
    Pair p(1, 2.0);
    static_assert(same<decltype(p), Pair<int, double>>::value);
    Holder h(5, 'x');
    static_assert(same<decltype(h), Holder<int>>::value);
    Defaulted d{};
    static_assert(same<decltype(d), Defaulted<long>>::value);
    if (direct.v != 3 || list.v != 'a' || copied.v != 4 || again.v != 3 || same_list.v != 3) return 1;
    if (p.first != 1 || p.second != 2.0 || h.v != 5 || d.v != 9 || constant.v != 7 || ns.v != 1.5) return 2;
    return 0;
}
