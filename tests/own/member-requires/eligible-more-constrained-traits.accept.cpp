// #1417 (the triviality checks of eligible-more-constrained.run.cpp) [special], [class.prop], [temp.constr.order], [over.match.best.general]: when two
// default constructors are both satisfied, the less constrained one is not eligible; the more
// constrained `= default` one is selected and makes the class trivially default constructible.
// XFAIL: ycxx type-trait built-ins do not classify class template specializations yet (#218)
template<class T> concept Small = sizeof(T) <= 4;
template<class T> concept SmallSigned = Small<T> && (T(-1) < T(0));

template<class T>
struct D {
  T v;
  int mark;
  D() requires Small<T> : v(), mark(1) {}
  D() requires SmallSigned<T> = default;
};

static_assert(__is_trivially_constructible(D<int>));
static_assert(!__is_trivially_constructible(D<unsigned>));

D<int> zeroed;   // static storage: zero-initialized; the trivial constructor leaves mark at 0

int main() {
  D<unsigned> u;
  if (u.mark != 1 || u.v != 0) return 1;
  if (zeroed.mark != 0) return 2;
  return 0;
}
