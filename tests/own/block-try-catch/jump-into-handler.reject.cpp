// [except.pre] A goto cannot jump from the try block into its handler.
// Keep the jump and label on one line because compilers can diagnose either.
void f() {
  try { goto handler_label; } catch (...) { handler_label: return; } //! error
}
