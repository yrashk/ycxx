// [except.handle] Instantiating the handler with T = int gives a forbidden rvalue reference.
template <class T>
void handle() {
  try { throw 1; }
  catch (T&&) {} //! error
}

void f() { handle<int>(); }
