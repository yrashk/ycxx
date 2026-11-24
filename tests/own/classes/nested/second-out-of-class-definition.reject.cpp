// #306 [basic.def.odr]/1: a member class defined out of class is defined once.
struct O { struct I; };
struct O::I { int a = 1; };
struct O::I { int a = 5; }; //! error
