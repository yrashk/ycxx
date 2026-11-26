struct Pair {
  unsigned int first;
  unsigned int second;
};

Pair cast(unsigned int source) {
  return __builtin_bit_cast(Pair, source); //! error
}
