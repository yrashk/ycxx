// #1400: empty pack expansions in mem-initializers ([temp.variadic]: an empty expansion does
// not alter the syntactic interpretation of the construct). `value(args...)` with no arguments
// value-initializes the member ([dcl.init.general], `()` initializer): a scalar becomes zero and
// a class is default-constructed; and `value{args...}` becomes `value{}`. A base pack's empty
// `Bs(x)...` is in base-pack-empty.run.cpp.
// EXIT: 0
struct Counted { int v; Counted() : v(77) {} Counted(int x) : v(x) {} };
struct Agg { int a, b; };

template<class... A> struct ParenScalar {
  int value = 5;
  ParenScalar(A... args) : value(args...) {}
};
template<class... A> struct ParenClass {
  Counted value;
  ParenClass(A... args) : value(args...) {}
};
template<class... A> struct BracedAgg {
  Agg value;
  BracedAgg(A... args) : value{args...} {}
};
template<class... A> struct BracedScalar {
  int value = 5;
  BracedScalar(A... args) : value{args...} {}
};

int main() {
  ParenScalar<> scalar;
  if (scalar.value != 0) return 1;
  ParenClass<> klass;
  if (klass.value.v != 77) return 2;
  BracedAgg<> agg;
  if (agg.value.a != 0 || agg.value.b != 0) return 3;
  BracedScalar<> braced;
  if (braced.value != 0) return 4;
  return 0;
}
