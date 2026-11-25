struct Nontrivial {
  unsigned int value;
  ~Nontrivial() {}
};

void cast_to_nontrivial(unsigned int source) {
  __builtin_bit_cast(Nontrivial, source); //! error
}

unsigned int cast_from_nontrivial(const Nontrivial& source) {
  return __builtin_bit_cast(unsigned int, source); //! error
}
