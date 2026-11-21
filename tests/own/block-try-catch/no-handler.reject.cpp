// [except.pre] The grammar requires at least one handler.
// Keep the try and following brace on one line for diagnostic locations.
void f() {
  try {} } //! error
