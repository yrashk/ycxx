struct Incomplete;

void cast_to_incomplete(unsigned int source) {
  __builtin_bit_cast(Incomplete, source); //! error
}

unsigned int cast_from_incomplete(const Incomplete& source) {
  return __builtin_bit_cast(unsigned int, source); //! error
}
