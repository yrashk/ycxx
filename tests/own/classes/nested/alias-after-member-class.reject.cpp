// #306 [basic.scope.scope]/4: a member class and a typedef-name of the same name conflict.
struct O {
  struct I {};
  using I = int; //! error
};
