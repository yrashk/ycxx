struct Format { int precision; };
template<int P> inline constexpr Format format_of{P};

template<int P> int evaluate() {
  constexpr Format local = format_of<P>;
  constexpr int result = [] { return local.precision; }();
  static_assert(result == P);
  return result;
}
