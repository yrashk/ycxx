// #340: a static data member template whose initializer is ill-formed for some arguments is no
// error while only valid specializations are used, in a class and in a class template
// ([temp.inst]/7, [temp.mem]).
struct C {
  template <class T> static constexpr int get = T::value;
};
template <class A> struct D {
  template <class T> static constexpr int get = A::value + T::value;
  static constexpr int fine = 1;
};
struct Has { static constexpr int value = 3; };
static_assert(C::get<Has> == 3);
static_assert(D<Has>::get<Has> == 6);
static_assert(D<int>::fine == 1);
