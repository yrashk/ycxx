// [except.pre] Switch dispatch cannot enter a protected block from outside it.
void f(int n) {
  switch (n) {
    try {
    default: //! error
      break;
    } catch (...) {}
  }
}
