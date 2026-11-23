// [temp.dep.constexpr], [dcl.enum]
template<int Start> struct Schedule {
  static constexpr int seed = Start;

  enum class Slot : unsigned short {
    first = Schedule::seed,
    next = Slot::first + 3,
    last,
  };
};

using EarlySlot = Schedule<4>::Slot;
using LateSlot = Schedule<18>::Slot;
static_assert(sizeof(EarlySlot) == sizeof(unsigned short));
static_assert(sizeof(LateSlot) == sizeof(unsigned short));

constexpr EarlySlot early = EarlySlot::last;
constexpr LateSlot late = LateSlot::last;
static_assert(static_cast<unsigned short>(EarlySlot::first) == 4);
static_assert(static_cast<unsigned short>(LateSlot::next) == 21);
static_assert(static_cast<unsigned short>(early) == 8);
static_assert(static_cast<unsigned short>(late) == 22);
