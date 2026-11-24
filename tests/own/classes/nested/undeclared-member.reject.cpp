// #306 [class.pre]/3: a qualified class-head-name names a class declared in the class.
struct O {};
struct O::I {}; //! error
