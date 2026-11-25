template<class> using Ignore = int;
template<class T> using DirectCode = typename T::code; //! error

struct NoCode {};
using Invalid = Ignore<DirectCode<NoCode>>;
