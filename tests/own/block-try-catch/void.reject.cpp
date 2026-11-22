// [except.handle] A void exception declaration is ill-formed.
void f() {
  try {}
  catch (void) {} //! error
}
