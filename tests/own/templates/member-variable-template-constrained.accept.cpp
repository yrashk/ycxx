// #340: a constrained static data member template, used with satisfied constraints, in a class
// and in a class template ([temp.mem], [temp.constr.decl], [temp.names]/8).
template <class T> concept Small = sizeof(T) <= 4;
struct C {
  template <class T> requires (sizeof(T) == 1) static constexpr bool one = true;
  template <Small T> static constexpr int size = int(sizeof(T));
};
template <class A> struct D {
  template <class T> requires (sizeof(T) <= sizeof(A)) static constexpr bool c = true;
  static constexpr bool own = c<char>;
};
static_assert(C::one<char>);
static_assert(C::size<short> == 2);
static_assert(D<int>::c<short>);
static_assert(D<long long>::own);
