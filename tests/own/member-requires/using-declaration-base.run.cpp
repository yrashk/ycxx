// #1417 [namespace.udecl], [over.match.funcs.general], [over.match.viable]: a derived member
// with a trailing requires-clause does not correspond to the unconstrained base member a
// using-declaration names, so it does not hide it; when the derived constraint is not
// satisfied, the derived member is not viable and the base member is called.
template<class T> concept Small = sizeof(T) <= 2;

struct Base {
  int get() const { return 1; }
  int other(int x) const { return x; }
};

template<class T>
struct Derived : Base {
  using Base::get;
  using Base::other;
  int get() const requires Small<T> { return 2; }
  int other(long x) const requires Small<T> { return int(x) * 2; }
};

int main() {
  Derived<int> big;
  if (big.get() != 1) return 1;
  if (big.other(5L) != 5) return 2;   // derived unsatisfied: base other(int) after conversion
  Derived<char> small;
  if (small.other(5L) != 10) return 3;   // different parameter types: exact match wins
  if (small.other(5) != 5) return 4;
  return 0;
}
