template<class> using Ignore = int;
struct NoCode {};

// Compilers diagnose the alias definition or the use, so both share the marked line.
template<class T> using DirectCode = typename T::code; using Invalid = Ignore<DirectCode<NoCode>>; //! error
