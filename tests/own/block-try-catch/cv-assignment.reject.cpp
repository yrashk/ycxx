// [except.handle] A named const-reference handler cannot modify the referred object.
void f() {
  try {}
  catch (const int& value) {
    value = 1; //! error
  }
}
