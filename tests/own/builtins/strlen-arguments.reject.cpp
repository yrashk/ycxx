decltype(sizeof(0)) length_with_no_argument() {
  return __builtin_strlen(); //! error
}

decltype(sizeof(0)) length_with_extra_argument() {
  return __builtin_strlen("abc", 0); //! error
}

decltype(sizeof(0)) length_of_ints(const int* input) {
  return __builtin_strlen(input); //! error
}

decltype(sizeof(0)) length_of_volatile_bytes(volatile char* input) {
  return __builtin_strlen(input); //! error
}
