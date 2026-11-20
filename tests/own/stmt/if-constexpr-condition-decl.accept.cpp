// #344: the decision variable is in scope in either arm. The condition's
// value is contextually converted to bool ([stmt.pre], [stmt.if]).
template<int N>
constexpr int adjust() {
  if constexpr (constexpr int value = N) {
    static_assert(value != 0);
    return value + 4;
  } else {
    static_assert(value == 0);
    return value - 5;
  }
}

static_assert(adjust<3>() == 7);
static_assert(adjust<0>() == -5);

// The init-statement's variable is available to the condition declaration
// and both variables are available to its substatements ([stmt.if]).
constexpr int with_init() {
  if constexpr (constexpr int base = 8; constexpr int count = base - 3) {
    return base + count;
  } else {
    return base - count;
  }
}

static_assert(with_init() == 13);
