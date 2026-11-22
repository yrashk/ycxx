// #344: the converted condition must be a constant expression. A mutable
// decision variable does not qualify, even with a constant initializer
// ([stmt.if], [expr.const]).
void check() {
  if constexpr (const bool constant = true) {
    static_assert(constant);
  }
  if constexpr (bool mutable_value = true) {} //! error
}
