struct Pair {
  unsigned int first;
  unsigned int second;
};

Pair cast(unsigned int source) {
  return __builtin_bit_cast(Pair, source); //! error
}

unsigned long long cast_int(int source) {
  return __builtin_bit_cast(unsigned long long, source); //! error
}
