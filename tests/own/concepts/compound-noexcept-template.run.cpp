// #775: `{ e } noexcept;` where e calls function template specializations and a class-template
// member, as libycxx's __nothrow_swappable_with_ and __nothrow_invocable_ do with std::swap and
// invoke ([expr.prim.req.compound]/2, [except.spec]/13).
// EXIT: 0
template<class T> constexpr bool nothrow_move = true;
template<class T> void sw(T& a, T& b) noexcept(nothrow_move<T>) {}
template<class T> void plain(T) noexcept {}
template<bool B> void flag() noexcept(B) {}
template<class T> struct W { void f() noexcept(sizeof(T) > 1) {} };
template<class T, class U> concept NothrowSwap = requires(T&& t, U&& u) {
  { sw(static_cast<T&&>(t), static_cast<U&&>(u)) } noexcept;
};
static_assert(NothrowSwap<int&, int&>);
static_assert(requires { { plain(1) } noexcept; });
static_assert(requires { { flag<true>() } noexcept; } && !requires { { flag<false>() } noexcept; });
static_assert(requires(W<int>& w, W<char>& c) { { w.f() } noexcept; } && !requires(W<char>& c) { { c.f() } noexcept; });
int main() { return 0; }
