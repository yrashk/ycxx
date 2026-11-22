// #828: a function template specialization, and a member of a class template specialization,
// has the exception specification its declaration gives, with a dependent condition evaluated
// with the specialization's arguments when it is needed ([except.spec]/13, [temp.inst]). The
// unary `noexcept` operator reads it ([expr.unary.noexcept]). Each check agrees with Clang and GCC.
// EXIT: 0
template<class T> void bare(T) noexcept {}
template<class T> void plain(T) {}
template<bool B> void value() noexcept(B) {}
template<class T> void sized() noexcept(sizeof(T) == 1) {}
template<class T> void param(T t) noexcept(noexcept(t + 1)) {}
template<class T> struct W {
  void f() noexcept(sizeof(T) > 1) {}
  static void g() noexcept(sizeof(T) == 1) {}
  void h() noexcept {}
  void k() {}
};
// A non-dependent noexcept on a pointer's function type in a template (#819).
int nothrow_fn() noexcept { return 0; }
template<class T> int pointer() {
  int (*p)() noexcept = nothrow_fn;
  return noexcept(p()) ? 1 : 0;
}
// One specialization, whether called or its address taken ([temp.inst]): one static local.
namespace {
template<class T> int counted(T) noexcept { static int n; return ++n; }
}
// A function parameter pack in the condition, as an invoker's ([temp.variadic]).
int nothrow_call(int, long) noexcept { return 0; }
int may_throw_call() { return 0; }
template<class F, class... A> void invoke(F&& f, A&&... a) noexcept(noexcept(static_cast<F&&>(f)(static_cast<A&&>(a)...))) {}
// Argument-dependent lookup at the point of instantiation, as `swap`'s ([temp.dep.candidate]).
template<class T> void adl(T& t) noexcept(noexcept(lookup(t))) {}
namespace user {
struct Good {};
void lookup(Good&) noexcept {}
struct Bad {};
void lookup(Bad&) {}
}
// A member's condition naming another member's, directly and through `this`.
template<class T> struct M {
  void b() noexcept(sizeof(T) == 4) {}
  static void c() noexcept(sizeof(T) == 4) {}
  void a() noexcept(noexcept(this->b())) {}
  void d() noexcept(noexcept(c())) {}
};
// A condition naming another specialization's.
template<class T> void outer() noexcept(noexcept(sized<T>())) {}

int main() {
  if (!noexcept(bare(1)) || noexcept(plain(1))) return 1;
  if (!noexcept(value<true>()) || noexcept(value<false>())) return 2;
  if (!noexcept(sized<char>()) || noexcept(sized<int>())) return 3;
  if (!noexcept(param(1))) return 4;
  W<int> wi;
  W<char> wc;
  if (!noexcept(wi.f()) || noexcept(wc.f())) return 5;
  if (!noexcept(W<char>::g()) || noexcept(W<int>::g())) return 6;
  if (!noexcept(wi.h()) || noexcept(wi.k())) return 7;
  if (!noexcept(outer<char>()) || noexcept(outer<int>())) return 8;
  if (pointer<int>() != 1) return 9;
  int first = counted(1);
  int (*count)(int) = counted;
  if (first != 1 || count(1) != 2) return 10;
  if (!noexcept(invoke(nothrow_call, 1, 2L)) || noexcept(invoke(may_throw_call))) return 11;
  user::Good good;
  user::Bad bad;
  if (!noexcept(adl(good)) || noexcept(adl(bad))) return 12;
  M<int> mi;
  M<char> mc;
  if (!noexcept(mi.a()) || noexcept(mc.a()) || !noexcept(mi.d()) || noexcept(mc.d())) return 13;
  // The pointer to a non-throwing specialization is a pointer to noexcept function.
  void (*p)() noexcept = sized<char>;
  void (*q)() noexcept = value<true>;
  p();
  q();
  return 0;
}
