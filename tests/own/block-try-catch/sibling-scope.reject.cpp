// [basic.scope.block] A named handler parameter is invisible in a sibling handler.
int f() {
  try { throw 1; }
  catch (double value) { return value > 0.0; }
  catch (...) {
    return value; //! error
  }
}
