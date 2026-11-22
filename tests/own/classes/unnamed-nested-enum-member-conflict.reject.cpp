// [dcl.enum], [basic.scope.scope]
struct Collision {
  static constexpr int repeated = 5;
  enum { repeated = 6 }; //! error
};
