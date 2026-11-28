// #340 item 3: the conversion function templates of libycxx's ranges result types
// ([algorithms.results]) and of `basic_const_iterator` ([const.iterators.iterator]): constrained
// by concepts, ref-qualified, and used in a return statement and in copy-initialization.
// EXIT: 0
template <class From, class To> concept convertible_to = requires(From (&f)()) { static_cast<To>(f()); };
template <class I, class F> struct in_fun_result {
  I in; F fun;
  template <class I2, class F2> requires convertible_to<const I&, I2> && convertible_to<const F&, F2>
  constexpr operator in_fun_result<I2, F2>() const& { return {in, fun}; }
  template <class I2, class F2> requires convertible_to<I, I2> && convertible_to<F, F2>
  constexpr operator in_fun_result<I2, F2>() && { return {static_cast<I&&>(in), static_cast<F&&>(fun)}; }
};
in_fun_result<long, long> f() { in_fun_result<int, short> r{1, 2}; return r; }

template <class T> concept ptr = requires(T t) { *t; };
template <class It> struct CI {
  It cur;
  template <class C> requires ptr<C> && requires(It i) { C(i); } constexpr operator C() const& { return cur; }
  template <class C> requires ptr<C> && requires(It i) { C(i); } constexpr operator C() && { return static_cast<It&&>(cur); }
};

int main() {
  auto r = f();
  in_fun_result<int, int> x{3, 4};
  in_fun_result<long, int> y = x;
  int v = 4;
  CI<int*> c{&v};
  const int* p = c;
  const int* q = CI<int*>{&v};
  return r.in + r.fun + y.in + y.fun == 10 && *p + *q == 8 ? 0 : 1;
}
