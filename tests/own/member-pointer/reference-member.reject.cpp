// [dcl.mptr] (#238)
struct Owner;
using Reference = int&;
using Invalid = Reference Owner::*; //! error
