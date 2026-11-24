// #306 [basic.def.odr]/1: a member class is defined once.
struct O {
  struct I {};
  struct I {}; //! error
};
