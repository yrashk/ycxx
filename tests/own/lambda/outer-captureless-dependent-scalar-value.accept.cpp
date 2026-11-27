template<int P> inline constexpr int format_of = P;

template<int P> int evaluate() {
  constexpr int local = format_of<P>;
  constexpr int result = [] { return local; }();
  static_assert(result == P);
  return result;
}

int witness() { return evaluate<24>(); }
