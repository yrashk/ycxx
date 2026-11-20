// [over.literal]: a literal operator belongs to a namespace.
struct S {
  static int operator""_k(unsigned long long); //! error
};
