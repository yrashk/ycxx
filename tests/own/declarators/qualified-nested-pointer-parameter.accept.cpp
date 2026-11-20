// [dcl.fct], [dcl.ptr]: a nested function-pointer parameter can have a qualified type.
namespace N {
using R = int;
using P = short;
}

template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

using Inner = int(short);
using Outer = int(Inner *);

void f() {
  N::R (*p)(N::R (*)(N::P));
  static_assert(same_type<decltype(p), Outer *>);
}
