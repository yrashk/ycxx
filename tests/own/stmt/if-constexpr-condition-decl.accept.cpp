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

// A value-dependent init-statement can initialize a placeholder decision variable.
template<int N>
constexpr int with_dependent_init() {
  if constexpr (constexpr int base = N; constexpr auto selected = base) {
    static_assert(base == N && selected == N);
    return base + selected;
  } else {
    static_assert(base == 0 && selected == 0);
    return base + selected - 1;
  }
}

static_assert(with_dependent_init<4>() == 8);
static_assert(with_dependent_init<0>() == -1);
