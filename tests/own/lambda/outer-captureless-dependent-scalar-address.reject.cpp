template<int P> inline constexpr int format_of = P;

template<int P> int evaluate() {
  constexpr int local = format_of<P>;
  const int* address = [] { return &local; }(); //! error
  return *address;
}

int witness() { return evaluate<24>(); }
