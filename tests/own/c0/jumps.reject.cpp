// [stmt.break], [stmt.cont], [stmt.return]: jumps outside a loop, and return statements that do
// not match the function's return type.
void none() {
  return 1; //! error
}
int some() {
  return; //! error
}
int main() {
  break; //! error
  continue; //! error
  return 0;
}
