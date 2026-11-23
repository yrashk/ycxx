template<unsigned char N> struct C {
  enum E { a = N, next, before_size = sizeof(a), next_size = sizeof(next), c = a, again = sizeof(c) };
};
static_assert(C<1>::before_size == sizeof(unsigned char));
static_assert(C<1>::next_size == sizeof(unsigned char));
static_assert(C<1>::again == sizeof(unsigned char));
