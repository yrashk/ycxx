// #1065 ordinary name lookup applies to the operand of the GNU builtin.
int invalid_probe() {
  return __builtin_constant_p(undeclared + 1); //! error
}
