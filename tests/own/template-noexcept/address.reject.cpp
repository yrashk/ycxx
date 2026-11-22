// #828, #819: a specialization whose exception specification is potentially throwing, or a
// function that may throw, does not convert to a pointer to noexcept function ([conv.fctptr],
// [except.spec]/13). In a template, a non-dependent noexcept on a pointer's function type is
// part of it ([dcl.fct]/1).
template<class T> void sized() noexcept(sizeof(T) == 1) {}
int may_throw() { return 0; }
template<class T> int f() {
  int (*p)() noexcept = may_throw;  //! error
  return p();
}
int main() {
  void (*q)() noexcept = sized<int>;  //! error
  q();
  return f<int>();
}
