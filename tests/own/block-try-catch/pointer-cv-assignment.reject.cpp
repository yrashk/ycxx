// [except.handle] Top-level cv on a named pointer handler is preserved.
void f() {
  try {}
  catch (int* const pointer) {
    pointer = nullptr; //! error
  }
}
