// [except.handle] A handler cannot refer to an incomplete object type.
struct Incomplete;
void f() {
  try {}
  catch (const Incomplete&) {} //! error
}
