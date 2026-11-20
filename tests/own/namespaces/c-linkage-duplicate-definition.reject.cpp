// [dcl.link], [basic.def.odr] (#446)
// XFAIL: gcc GCC 16 accepts these definitions in syntax-only mode.
namespace A { extern "C" int same_function() { return 7; } }
namespace B { extern "C" int same_function() { return 7; } } //! error
