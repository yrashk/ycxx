// [except.handle] A duplicate typed handler must still be checked.
int f() {
  try { throw 1; }
  catch (int value) { return value; }
  catch (int duplicate) {
    return missing; //! error
  }
}
