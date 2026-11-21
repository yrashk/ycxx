// [except.handle] Pointers to incomplete types are forbidden, except pointers to cv void.
struct Incomplete;
void f() {
  try {}
  catch (const Incomplete*) {} //! error
}
