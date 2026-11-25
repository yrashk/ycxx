// #306 [class.pre]/1: a class-head-name ends in an identifier.
struct O { struct I; };
struct O:: { int v; }; //! error
