// #1178, [expr.unary.noexcept], [except.spec], [temp.friend]: noexcept(...) reports the selected
// hidden friend's exception specification, including a dependent one.
namespace n {
struct A {
    enum class E { e };
    friend A operator+(const A&) noexcept;
    friend A operator~(const A&);
    friend bool operator!(const A&) noexcept(false);
    friend int operator*(A&) noexcept(true);
    friend int operator*(const A&) noexcept(sizeof(int) == 1);
    friend A& operator++(A&) noexcept;
    friend A& operator--(A&);
    friend A& operator+=(A&, int) noexcept;
    friend A& operator+=(A&, double);
    friend E& operator|=(E&, E) noexcept;
    friend E operator~(E);
};
struct Thrower {
    friend Thrower operator~(const Thrower&);
    friend Thrower& operator+=(Thrower&, const Thrower&);
};
template <class T> struct W {
    T t;
    friend constexpr W operator~(const W& w) noexcept(noexcept(~w.t)) { return {~w.t}; }
    friend constexpr W& operator+=(W& a, const W& b) noexcept(noexcept(a.t += b.t)) { a.t += b.t; return a; }
    friend constexpr W& operator++(W& a) noexcept(sizeof(T) > 1) { ++a.t; return a; }
};
}
n::A a;
const n::A c{};
n::A::E e;
static_assert(noexcept(+a) && !noexcept(~a) && !noexcept(!a));
static_assert(noexcept(*a) && !noexcept(*c));
static_assert(noexcept(++a) && !noexcept(--a));
static_assert(noexcept(a += 1) && !noexcept(a += 1.0) && !noexcept(--a += 1) && !noexcept(+~a));
static_assert(noexcept(e |= e) && !noexcept(e |= ~e));
n::W<int> wi;
n::W<char> wc;
n::W<n::Thrower> wt;
static_assert(noexcept(~wi) && !noexcept(~wt));
static_assert(noexcept(wi += wi) && !noexcept(wt += wt));
static_assert(noexcept(++wi) && !noexcept(++wc));
