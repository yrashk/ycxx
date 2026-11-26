// #157 [class.prop]: a deleted move assignment operator is part of what decides a class's layout on
// arm64 macOS, and ycxx does not support declaring one yet. It says so, and gives the class no layout.
// XFAIL: clang,gcc valid: ycxx does not support a deleted move assignment operator yet (#157)
struct Q { int i; char j; };
struct Constant { Constant() = default; Constant& operator=(Constant&&) = delete; const Q q; char c; }; //! error
struct Volatile { Volatile& operator=(Volatile&&) = delete; volatile Q q; char c; }; //! error
struct Copied { Copied(const Copied&) = default; Copied& operator=(Copied&&) = delete; int x; char c; }; //! error
struct Moved { Moved(Moved&&) = default; Moved& operator=(Moved&&) = delete; const Q q; char c; }; //! error
struct Deleted { Deleted(const Deleted&) = delete; Deleted& operator=(Deleted&&) = delete; const Q q; char c; }; //! error
template <class T> struct Template { Template& operator=(Template&&) = delete; T x; char c; }; //! error
