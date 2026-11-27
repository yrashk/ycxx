// #1418 [class.mem.general], [dcl.init.aggr], [expr.const]: a dependent-bound array member's
// default member initializer in constant evaluation.
// XFAIL: ycxx the interpreter does not evaluate an object with an array member (#1140)
// EXIT: 0

template <int N> struct Wd { unsigned long long w[N] = {}; };
template <int N> struct Partial { int w[N] = {1, 2}; };
template <class T, int N> struct Typed { T w[N] = {}; };

template <class T> constexpr Wd<2> make(T) {
  Wd<2> r;
  r.w[0] = 1;
  return r;
}
static_assert(make(1.0).w[0] == 1 && make(1.0).w[1] == 0);

constexpr Partial<4> table{};
static_assert(table.w[0] == 1 && table.w[1] == 2 && table.w[3] == 0);
static_assert(Partial<3>{}.w[2] == 0);
static_assert(Typed<long, 2>{}.w[1] == 0);

constexpr int sum() {
  Partial<4> p;
  p.w[3] += 5;
  return p.w[0] + p.w[1] + p.w[2] + p.w[3];
}
static_assert(sum() == 8);

int main() { return make(1).w[1] != 0; }
