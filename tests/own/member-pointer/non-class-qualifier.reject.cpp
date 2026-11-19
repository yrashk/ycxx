// [dcl.mptr] (#238)
enum class Kind { value };
using Invalid = int Kind::*; //! error
