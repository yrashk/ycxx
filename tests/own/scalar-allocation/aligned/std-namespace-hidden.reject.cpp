// [basic.stc.dynamic] The built-in allocation declarations do not introduce namespace std.
// XFAIL: gcc GCC exposes namespace std without a standard library declaration.
namespace BeforeLibrary = std; //! error
