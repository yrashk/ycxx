template<int P> inline constexpr int format_of = P;

template<int P> int evaluate() {
  constexpr int local = format_of<P>;
  return [] { const int& reference = local; return reference; }(); //! error
}

int witness() { return evaluate<24>(); }
