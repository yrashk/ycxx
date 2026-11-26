// XFAIL: gcc GCC 16.2 rejects a discarded bit cast in constant evaluation (#1531)
constexpr int count(int value) {
  (void)__builtin_bit_cast(unsigned int, ++value);
  return value;
}
static_assert(count(-7) == -6);
