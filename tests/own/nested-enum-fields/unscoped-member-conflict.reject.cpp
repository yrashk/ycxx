// [dcl.enum], [basic.scope.scope]
struct Box {
  enum State { active };
  int active() const { return 0; }
};
