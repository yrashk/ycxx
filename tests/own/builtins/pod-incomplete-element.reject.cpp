struct Incomplete;
using Elements = Incomplete[][2];

constexpr bool pod = __is_pod(Elements); //! error
