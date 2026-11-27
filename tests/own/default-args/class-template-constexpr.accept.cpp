// #356: constexpr member functions and constructors of a class template whose dependent default
// arguments are used in constant expressions ([dcl.constexpr], [expr.const], [dcl.fct.default],
// [temp.inst]), including a default naming a later static member ([class.mem.general]).
template<class T, int N>
struct Calc {
  constexpr Calc(T x = T(N) * 2) : v(x) {}
  constexpr T scaled(T k = factor + T(N)) const { return v * k; }
  static constexpr T id(T x = T(sizeof(T))) { return x; }
  static constexpr T factor = 3;
  T v;
};

static_assert(Calc<int, 5>{}.v == 10);
static_assert(Calc<int, 5>{}.scaled() == 80);
static_assert(Calc<int, 5>{4}.scaled(2) == 8);
static_assert(Calc<long, 1>::id() == static_cast<long>(sizeof(long)));
static_assert(Calc<char, 0>::id(9) == 9);
constexpr Calc<short, 2> c{};
static_assert(c.scaled() == 20);
