// [expr.cond] The throw operand rule permits parentheses around the throw-expression.
// XFAIL: gcc GCC 16 accepts the void cast as if it were a parenthesized throw-expression.
void select(bool choose) {
  choose ? 42 : (static_cast<void>(throw 1)); //! error
}
