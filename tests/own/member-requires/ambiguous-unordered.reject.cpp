// #1417 [over.match.best.general], [temp.constr.order], [over.match.viable]: two satisfied
// member functions whose constraints are not ordered by subsumption (different concepts) are
// equally good, so the call is ambiguous.
template<class T> concept Small = sizeof(T) <= 4;
template<class T> concept Signed = T(-1) < T(0);

template<class T>
struct Two {
  int f() requires Small<T> { return 1; }
  int f() requires Signed<T> { return 2; }
};

int main() {
  Two<unsigned> u;
  Two<long long> l;
  Two<int> i;
  return u.f() + l.f() + i.f(); //! error
}
