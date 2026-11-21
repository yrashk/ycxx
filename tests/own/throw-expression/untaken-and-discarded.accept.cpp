// [expr.const], [stmt.if] Neither constant evaluation executes a throw.
constexpr int select(bool fail) {
  if (fail) throw 18;
  return 27;
}

constexpr int discarded() {
  if constexpr (false) throw 22;
  return 39;
}

static_assert(select(false) == 27);
static_assert(discarded() == 39);
