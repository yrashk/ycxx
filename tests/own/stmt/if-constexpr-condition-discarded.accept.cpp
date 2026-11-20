// #344, #241: instantiation discards the unselected arm, so its false
// assertion has no effect ([stmt.if], [dcl.pre]).
template<int N>
constexpr int select_then() {
  if constexpr (constexpr bool selected = N > 0) {
    return selected ? N : -1;
  } else {
    static_assert(false);
    return 0;
  }
}

template<int N>
constexpr int select_else() {
  if constexpr (constexpr bool selected = N != 0) {
    static_assert(false);
    return 0;
  } else {
    return selected ? -1 : N + 3;
  }
}

static_assert(select_then<5>() == 5);
static_assert(select_else<0>() == 3);
