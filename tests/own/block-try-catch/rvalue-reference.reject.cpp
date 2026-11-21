// [except.handle] An exception declaration cannot have rvalue reference type.
void f() {
  try {}
  catch (int&&) {} //! error
}
