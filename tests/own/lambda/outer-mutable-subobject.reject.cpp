struct Format { int precision; mutable int cache; };
int f() {
  constexpr Format local{24,0};
  return [] { return local.precision; }(); //! error
}
