template<class> using Ignore = int;
template<class T> using Code = Ignore<typename T::code>; //! error

struct NoCode {};
using Invalid = Code<NoCode>;
