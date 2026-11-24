// #306 [class.nest]/3, [namespace.memdef]: a nested class must be defined in a namespace enclosing its class.
namespace N {
class O { public: class I; };
}
namespace M {
class N::O::I { };  //! error
}
