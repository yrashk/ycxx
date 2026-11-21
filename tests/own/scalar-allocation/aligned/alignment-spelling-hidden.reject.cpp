// [basic.stc.dynamic] The built-in declarations alone introduce no qualified enum name.
// XFAIL: gcc GCC exposes std::align_val_t without a standard library declaration.
namespace std {}
using EarlyAlignment = std::align_val_t; //! error
