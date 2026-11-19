// [dcl.mptr] (#238)
struct Owner;
using Invalid = const void Owner::*; //! error
