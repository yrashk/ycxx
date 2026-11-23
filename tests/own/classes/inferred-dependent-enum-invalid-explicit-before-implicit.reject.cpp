// [dcl.enum], [expr.const], [temp.res]
template<int Seed> struct Sequence {
  enum Step { first = Seed, second, invalid = 8 / 0, after_invalid }; //! error
};
static_assert(Sequence<4>::second == 5);
