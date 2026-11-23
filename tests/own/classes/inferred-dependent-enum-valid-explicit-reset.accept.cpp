// [dcl.enum], [temp.inst]
template<int Seed> struct Sequence {
  enum Step {
    first = Seed,
    second,
    reset = 8,
    after_reset
  };
};
static_assert(Sequence<4>::first == 4);
static_assert(Sequence<4>::second == 5);
static_assert(Sequence<4>::after_reset == 9);
