// [except.pre] A goto cannot enter a try block from outside it.
// Keep the jump and label on one line because compilers can diagnose either.
void f() {
  goto protected_label; try { protected_label: return; } catch (...) {} //! error
}
