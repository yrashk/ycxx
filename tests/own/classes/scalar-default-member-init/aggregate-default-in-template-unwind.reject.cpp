// XFAIL: clang Clang supports unwinding through the selected default.
// XFAIL: gcc GCC supports unwinding through the selected default.
int source();
struct Record { int value = source(); };

template<int Offset> int read() {
  Record named{}; //! error
  return named.value + Offset;
}

template<int Offset> int read_in_init_statement() {
  if constexpr (Record named{}; true) return named.value + Offset; //! error
}

int read_specialization() { return read<2>(); }
int read_init_specialization() { return read_in_init_statement<2>(); }
