// #1417 [dcl.constexpr], [over.match.viable], [class.default.ctor]: constexpr members and
// constructors with trailing requires-clauses are selected by their constraints during
// constant evaluation.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct K {
  int n;
  constexpr K() requires Small<T> : n(1) {}
  constexpr K() requires (!Small<T>) : n(2) {}
  constexpr int get() const requires Small<T> { return n * 10; }
  constexpr int get() const requires (!Small<T>) { return n * 100; }
};

static_assert(K<char>().get() == 10);
static_assert(K<int>().get() == 200);
