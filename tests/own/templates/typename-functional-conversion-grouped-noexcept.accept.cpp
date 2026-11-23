// #917 [dcl.meaning], [dcl.fct], [except.spec], [stmt.ambig].
struct Source { using type = long; };
long target(int value) noexcept { return value; }
template<class Provider>
long declarations() {
  typename Provider::type ((*implicit_true)(int) noexcept) = target;
  typename Provider::type ((*explicit_true)(int) noexcept(true)) = target;
  return implicit_true(1) + explicit_true(2);
}
long check() { return declarations<Source>(); }
