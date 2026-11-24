// #306 [basic.scope.scope]/4: a member class and an enumeration of the same name conflict.
struct O {
  struct I;
  enum I { A }; //! error
};
