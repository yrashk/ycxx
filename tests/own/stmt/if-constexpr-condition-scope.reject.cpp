// #344: the arm's outer block cannot redeclare its decision variable.
// A nested block can hide it, and its scope ends with the if ([basic.scope.block], [stmt.if]).
void valid_scopes() {
  if constexpr (constexpr int decision = 2) {
    {
      constexpr int decision = 3;
      static_assert(decision == 3);
    }
    static_assert(decision == 2);
  }
  constexpr int decision = 5;
  static_assert(decision == 5);
}

void redeclare() {
  if constexpr (constexpr bool decision = true) {
    int decision = 1; //! error
  }
}
