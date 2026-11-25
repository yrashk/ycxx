unsigned int object;

unsigned int cast(unsigned int source) {
  return __builtin_bit_cast(object, source); //! error
}
