// #306 [class.mem], [basic.def.odr]: defining a nested class out of class when it was already defined in-class.
class O { public: class I { public: int a; }; };
class O::I { public: int b; };  //! error
