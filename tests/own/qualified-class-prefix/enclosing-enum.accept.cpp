// [basic.scope.enum], [dcl.enum]
struct Box {
  static constexpr int value = 4;
  static constexpr int first = 7;
  enum class State { first = Box::value, second = State::first + 1, third = Box::first };
};
static_assert(static_cast<int>(Box::State::first) == 4);
static_assert(static_cast<int>(Box::State::second) == 5);
static_assert(static_cast<int>(Box::State::third) == 7);
