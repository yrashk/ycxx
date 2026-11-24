// #306 [class.mem.general]: a nested class does not have the name of its enclosing class.
struct O {
  struct O {}; //! error
};
