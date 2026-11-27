struct Format { int precision; };
template<int P> inline constexpr Format format_of{P};

template<int P> int evaluate() {
  constexpr Format local = format_of<P>;
  return [] { const int& reference = local.precision; return reference; }(); //! error
}

int witness() { return evaluate<24>(); }
