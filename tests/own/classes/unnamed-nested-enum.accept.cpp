// [dcl.enum], [basic.scope.enum], [basic.scope.class]
struct Fixed {
  static constexpr int base = 5;
  enum : unsigned char { start = base, finish = start + 1 };
};
static_assert(Fixed::start == 5 && Fixed::finish == 6);
static_assert(sizeof(decltype(Fixed::finish)) == sizeof(unsigned char));

struct Inferred {
  static constexpr int base = -3;
  enum { start = base, finish = start + 4 };
};
static_assert(Inferred::start == -3 && Inferred::finish == 1);

template<class T> struct Generic {
  static constexpr int base = 7;
  enum : short { first = base, second = first + 1 };
  enum { third = second + 1 };
};
static_assert(Generic<int>::first == 7 && Generic<int>::third == 9);
static_assert(sizeof(decltype(Generic<int>::second)) == sizeof(short));
