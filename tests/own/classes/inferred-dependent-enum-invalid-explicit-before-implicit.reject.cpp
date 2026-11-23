// [dcl.enum], [expr.const], [temp.res]
template<int Seed> struct Sequence {
  enum Step {
    first = Seed,
    second,
    invalid = 8 / 0, //! error
    after_invalid
  };
};
static_assert(Sequence<4>::second == 5);
