// [dcl.enum], [basic.scope.enum]
struct Box {
  enum class State { active = 17 } values[static_cast<int>(active)];
};
