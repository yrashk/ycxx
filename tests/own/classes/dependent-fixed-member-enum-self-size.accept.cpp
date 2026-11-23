template<int N> struct C { enum E : int { a = N, valid = sizeof(E) }; };
static_assert(C<1>::valid == sizeof(int));
