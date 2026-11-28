// [expr.type.conv]/1, [temp.dep.expr] (#286): a dependent decay-copy in a function template's
// trailing return type, noexcept-specifier and requires-clause, and in a class template's base.
struct It { int* p; };
struct R { It begin(); int* data(); };

template<class T> auto first(T& t) -> decltype(auto(t.begin())) { return t.begin(); }
template<class T> auto ptr(T& t) -> decltype(auto{t.data()}) { return t.data(); }
template<class T> int* nothrow_ptr(T& t) noexcept(noexcept(auto(t.data()))) { return t.data(); }
template<class T> int* nothrow_copy(T& t) noexcept(noexcept(auto(t))) { return t.p; }
template<class T> requires requires(T& t) { auto(t.begin()); } constexpr int constrained(T&) { return 1; }
constexpr int constrained(...) { return 2; }

template<class T> struct Base { using type = T; };
template<class T> struct Derived : Base<decltype(auto(T{}))> {};

static_assert(__is_same(Derived<const It>::type, It));

int use() {
    R r;
    int x = 0;
    It i = first(r);  // a static_assert on decltype(first(r)) needs #1599
    int* p = ptr(r);
    It it{};
    static_assert(!noexcept(nothrow_ptr(r)));
    static_assert(noexcept(nothrow_copy(it)));
    (void)i;
    (void)p;
    return constrained(r) + constrained(x);
}
