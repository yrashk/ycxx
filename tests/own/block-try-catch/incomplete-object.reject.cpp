// [except.handle] The exception declaration cannot have incomplete object type.
struct Incomplete;
void f() {
  try {}
  catch (Incomplete) {} //! error
}
