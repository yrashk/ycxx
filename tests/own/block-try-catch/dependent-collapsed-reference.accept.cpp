// [dcl.ref] Substituting an lvalue reference for T in T&& collapses it to T.
// [except.handle] An lvalue reference is permitted as a handler type.
template <class T>
int handle() {
  try { throw 1; }
  catch (T&& value) {
    int& reference = value;
    return reference;
  }
}

int f() { return handle<int&>(); }
