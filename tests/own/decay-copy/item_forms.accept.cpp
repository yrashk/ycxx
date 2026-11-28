// [expr.type.conv]/1 (#286): `auto(` and `auto{` in expression position at namespace scope and
// in class members, followed by each kind of next item.
constexpr int n = 3;
int a = auto(n);
int b = auto{n};
struct Copy { int v; };
constexpr Copy c{4};
constexpr Copy d = auto(c);
static_assert(d.v == 4);
decltype(auto(n)) e = 5;
int after = 6;

struct Members {
    int m = auto(n);
    int braced = auto{n};
    static constexpr int s = auto(n);
    decltype(auto(n)) typed = 1;
    int f() { return auto(m); }
    using type = decltype(auto(n));
    int g() noexcept(noexcept(auto(n)));
    int last;
};
static_assert(Members::s == 3);
static_assert(__is_same(Members::type, int));

template<class T>
struct Holder {
    T value = auto(T{});
    static constexpr bool copies = requires(T& t) { auto(t); };
    T get() const { return auto(value); }
    int next;
};
static_assert(Holder<int>::copies);
static_assert(Holder<Copy>::copies);
int use() { return Holder<int>{}.get(); }  // a class-typed default member needs #118
