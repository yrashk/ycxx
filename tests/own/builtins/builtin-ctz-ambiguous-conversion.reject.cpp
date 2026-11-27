struct Ambiguous {
  operator unsigned int() const;
  operator unsigned int&() const;
};

int count(Ambiguous value) {
  return __builtin_ctz(value); //! error
}
