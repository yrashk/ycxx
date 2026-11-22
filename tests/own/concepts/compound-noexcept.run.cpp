// #775: a compound requirement `{ e } noexcept;` holds only when e is not potentially-throwing
// ([expr.prim.req.compound]/2, [except.spec]): calls to functions with and without a
// non-throwing exception specification, a member call through a parameter, a temporary whose
// destructor may throw, with a return-type-requirement, and in a constraint that selects an
// overload, as libycxx's __nothrow_invocable_ is used.
// EXIT: 0
template<class A, class B> constexpr bool same = false;
template<class A> constexpr bool same<A, A> = true;
template<class T, class U> concept Same = same<T, U>;

void g() noexcept;
void h();
struct Safe { int f() noexcept { return 1; } };
struct Risky { int f() { return 2; } };
struct Throwing { ~Throwing() noexcept(false) {} };
struct Quiet {};

template<class T> concept NoThrowF = requires(T& t) { { t.f() } noexcept; };
template<class T> concept NoThrowMake = requires { { T() } noexcept; };
template<class T> concept IntF = requires(T& t) { { t.f() } noexcept -> Same<int>; };
static_assert(requires { { g() } noexcept; });
static_assert(!requires { { h() } noexcept; });
static_assert(requires { { h() }; });
static_assert(NoThrowF<Safe> && !NoThrowF<Risky>);
static_assert(NoThrowMake<Quiet> && !NoThrowMake<Throwing>);
static_assert(IntF<Safe> && !IntF<Risky>);

template<class T> int pick(T& t) requires NoThrowF<T> { return 10 + t.f(); }
template<class T> int pick(T& t) { return 20 + t.f(); }

int main() {
  Safe s;
  Risky r;
  if (pick(s) != 11 || pick(r) != 22) return 1;
  return 0;
}
void g() noexcept {}
void h() {}
