// ζ f1 (#88): an unqualified call with a dependent argument: the candidates are those found by
// ordinary lookup at the template's definition, plus those found by argument-dependent lookup
// at the point of instantiation. A function declared after the template is found for a class
// argument (its associated namespace is the global one); for an int argument only the earlier
// declaration is a candidate, even though a better one is declared later ([temp.res],
// [temp.dep.candidate], [basic.lookup.argdep], [temp.point]).
// EXIT: 0
int describe(long) { return 1; }

template<class T>
int call(const T& t) { return describe(t); }

template<class T>
int call_twice(const T& t) { return describe(t) + describe(t); }

struct Widget {
  int v;
};

int describe(const Widget& w) { return 10 + w.v; }
int describe(int) { return 3; }

int main() {
  Widget w{2};
  if (call(w) != 12) return 1;
  if (call_twice(w) != 24) return 2;
  if (call(5) != 1) return 3;
  if (call('c') != 1) return 4;
  if (describe(5) != 3) return 5;
  return 0;
}
