// [temp.dep.constexpr], [dcl.enum]
template<int Seed> struct Key {
  enum : unsigned short { first = Seed, next = first + 2, last };
};

using First = decltype(Key<4>::first);
using Second = decltype(Key<9>::first);
static_assert(__is_same(__underlying_type(First), unsigned short));
static_assert(__is_same(__underlying_type(Second), unsigned short));
static_assert(__is_same(First, decltype(Key<4>::last)));
static_assert(!__is_same(First, Second));
static_assert(Key<4>::last == 7);
static_assert(Key<9>::last == 12);
