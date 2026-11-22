// XFAIL: clang Clang supports unwinding through the selected default.
// XFAIL: gcc GCC supports unwinding through the selected default.
int source();
struct Record { int value = source(); };

template<int Offset> int read() {
  Record named{}; //! error
  return named.value + Offset;
}

int read_specialization() { return read<2>(); }
