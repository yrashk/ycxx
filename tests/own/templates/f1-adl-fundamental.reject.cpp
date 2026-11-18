// ζ f1 (#88): a dependent call finds the functions visible at the template's definition plus
// those found by argument-dependent lookup; an int argument has no associated namespaces, so
// describe, declared after the template, is not found when call<int> is instantiated
// ([temp.dep.candidate], [basic.lookup.argdep], [temp.res]).
template<class T>
int call(T t) {
  return describe(t); //! error
}

int describe(int) { return 1; }

int main() {
  return call(5);
}
