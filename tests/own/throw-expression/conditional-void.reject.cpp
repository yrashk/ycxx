// [expr.cond] Ordinary void expressions do not receive the throw operand rule.
void nothing();

void select(bool choose) {
  choose ? 42 : nothing(); //! error
}
