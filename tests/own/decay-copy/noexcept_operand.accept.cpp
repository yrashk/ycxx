// [expr.type.conv]/1, [expr.unary.noexcept] (#286): the copy of a class operand is part of
// `noexcept(auto(e))`, as the ranges CPOs' nothrow checks use it.
struct Nothrow { Nothrow() = default; Nothrow(const Nothrow&) noexcept {} };
struct Throwing { Throwing() = default; Throwing(const Throwing&) {} };

Nothrow& nothrow_lvalue() noexcept;
Throwing& throwing_lvalue() noexcept;
Throwing throwing_prvalue() noexcept;

static_assert(noexcept(auto(nothrow_lvalue())));
static_assert(!noexcept(auto(throwing_lvalue())));
static_assert(!noexcept(auto{throwing_lvalue()}));
static_assert(noexcept(auto(throwing_prvalue())));  // no copy

template<class T> T& declval() noexcept;

struct Fn {
    template<class T>
    static consteval bool nothrow() { return noexcept(auto(declval<T&>())); }
};
static_assert(Fn::nothrow<Nothrow>());
static_assert(!Fn::nothrow<Throwing>());
static_assert(Fn::nothrow<int*>());

template<class T>
concept nothrow_copy = requires(T& t) { { auto(t) } noexcept; };
static_assert(nothrow_copy<Nothrow>);
static_assert(!nothrow_copy<Throwing>);
