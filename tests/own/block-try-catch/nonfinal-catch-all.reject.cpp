// [except.handle] An ellipsis handler must be last.
void work();
void f() {
  try { work(); }
  catch (...) {} //! error
  catch (int) {}
}
