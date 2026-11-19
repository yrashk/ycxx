// [dcl.mptr] (#238)
struct Owner;
using Invalid = void Owner::*; //! error
