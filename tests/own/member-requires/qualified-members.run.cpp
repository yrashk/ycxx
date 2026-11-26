// #1417 [class.mem.general], [dcl.decl.general], [over.match.viable], [over.match.funcs]:
// const-qualified and ref-qualified member functions with trailing requires-clauses; the
// requires-clause follows the cv- and ref-qualifiers, and an unsatisfied overload is skipped
// even when its qualifiers would match the object better.
template<class T> concept Wide = sizeof(T) >= 4;

template<class T>
struct Q {
  int get() const requires Wide<T> { return 1; }
  int get() requires (!Wide<T>) { return 2; }

  int ref() & requires Wide<T> { return 3; }
  int ref() && requires Wide<T> { return 4; }
  int ref() const & { return 5; }
};

int main() {
  Q<int> wi;
  Q<char> nc;
  const Q<int> cwi{};
  if (wi.get() != 1) return 1;    // only the const overload is viable
  if (cwi.get() != 1) return 2;
  if (nc.get() != 2) return 3;
  if (wi.ref() != 3) return 4;
  if (Q<int>{}.ref() != 4) return 5;
  if (nc.ref() != 5) return 6;    // & and && are unsatisfied for char
  if (Q<char>{}.ref() != 5) return 7;
  return 0;
}
