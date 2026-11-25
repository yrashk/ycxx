// A lambda in a constraint-expression (#338): its closure type belongs to the satisfaction that
// is checked, and its call's deduction decides it ([temp.constr.atomic], [expr.prim.req.simple]).
template <class>
struct O {};
template <class T>
concept IsO = requires(const T& t) { []<class U>(const O<U>&) {}(t); };
static_assert(IsO<O<int>> && !IsO<int> && IsO<const O<char>>);
namespace N {
template <class T>
struct B {};
template <class T>
concept PointsToB = requires(T* p) { []<class U>(const volatile B<U>*) {}(p); };
template <class T>
concept FourBytes = [] { return sizeof(T) == 4; }();
template <class T>
concept FourBytesGeneric = [](auto x) { return sizeof(x) == 4; }(T());
template <class T>
concept Always = [] { return true; }();
template <class T>
concept Nested = requires {
  [] { return true; }();
  requires [] { return sizeof(T) == 4; }();
};
// Two atoms of one concept, each with a lambda of the same signature.
template <class T>
concept Two = [] { return sizeof(T) > 1; }() && [] { return sizeof(T) < 8; }();
}
static_assert(N::PointsToB<N::B<int>> && !N::PointsToB<int>);
static_assert(N::FourBytes<int> && !N::FourBytes<char> && N::FourBytesGeneric<int> && !N::FourBytesGeneric<char>);
static_assert(N::Always<int> && N::Nested<int> && !N::Nested<char>);
static_assert(!N::Two<char> && N::Two<int> && !N::Two<long>);
template <class T>
  requires requires(T t) { [](int) {}(t); }
constexpr int f(T) { return 1; }
template <class T>
constexpr int f(T*) { return 2; }
template <class T>
constexpr int g(T) requires([] { return sizeof(T) == 4; }()) { return 3; }
template <class T>
constexpr int g(T) requires([] { return sizeof(T) != 4; }()) { return 4; }
template <IsO T>
constexpr int h(T) { return 5; }
template <class T>
constexpr int h(T) { return 6; }
int main() {
  int x = 0;
  char c = 0;
  if (f(x) != 1 || f(&x) != 2 || g(x) != 3 || g(c) != 4) return 1;
  if (h(O<int>()) != 5 || h(x) != 6) return 2;
  return 0;
}
