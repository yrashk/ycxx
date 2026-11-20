// [stmt.ambig], [expr.type.conv]: a declaration takes precedence where both readings are possible.
namespace api { using Result = int; }
template<class A, class B> constexpr bool same_type = false;
template<class T> constexpr bool same_type<T, T> = true;

constexpr bool probe() {
  int value = 3;
  int *p = &value;
  {
    api::Result (*p)(int);
    static_assert(same_type<decltype(p), int (*)(int)>);
  }
  api::Result (*p) ? ++value : --value;
  return value == 4;
}
static_assert(probe());
