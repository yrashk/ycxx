// #1417 [over.match.best.general], [temp.constr.order], [namespace.udecl]: a satisfied
// constrained derived member and an unconstrained base member named by a using-declaration
// are not ordered by constraints, because a non-template function is more
// partial-ordering-constrained than another only if, being member functions, both are
// direct members of the same class ([temp.constr.order]). The call is ambiguous.
// XFAIL: ycxx member using-declarations are not supported yet (#1150)
template<class T> concept Small = sizeof(T) <= 2;

struct Base {
  int get() const { return 1; }
};

template<class T>
struct Derived : Base {
  using Base::get;
  int get() const requires Small<T> { return 2; }
};

int main() {
  Derived<int> big;
  Derived<char> small;
  return big.get() + small.get(); //! error
}
