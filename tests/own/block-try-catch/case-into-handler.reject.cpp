// [except.pre] Switch dispatch cannot enter a handler from outside it.
void f(int n) {
  switch (n) {
    try {}
    catch (...) {
    case 0: //! error
      break;
    }
  }
}
