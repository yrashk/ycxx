struct Format { int precision; };
template<int P> inline constexpr Format format_of{P};

template<int P> int evaluate() {
  constexpr Format local = format_of<P>;
  const int* address = [] { return &local.precision; }(); //! error
  return *address;
}

int witness() { return evaluate<24>(); }
