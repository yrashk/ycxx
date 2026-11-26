// #1417 [class.default.ctor], [special], [class.prop], [dcl.fct.def.default]: a constrained
// defaulted default constructor (`S() requires C<T> = default;`) next to a constrained
// user-provided one; the eligible one decides both behavior and triviality.
// XFAIL: ycxx type-trait built-ins do not classify class template specializations yet (#218)
template<class T> concept Trivial = __is_trivially_constructible(T);

struct Counted {
  int n;
  Counted() : n(42) {}
};

template<class T>
struct Slot {
  T value;
  int mark;
  Slot() requires Trivial<T> = default;
  Slot() requires (!Trivial<T>) : value(), mark(7) {}
};

static_assert(__is_trivially_constructible(Slot<int>));
static_assert(!__is_trivially_constructible(Slot<Counted>));
static_assert(__is_constructible(Slot<int>));
static_assert(__is_constructible(Slot<Counted>));

Slot<int> a;   // static storage: zero-initialized, then the defaulted constructor does nothing

int main() {
  Slot<Counted> b;
  if (a.value != 0 || a.mark != 0) return 1;
  if (b.value.n != 42 || b.mark != 7) return 2;
  return 0;
}
