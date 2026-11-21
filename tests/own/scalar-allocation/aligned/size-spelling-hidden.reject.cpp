// [basic.stc.dynamic] The built-in allocation declarations do not introduce std::size_t.
namespace std {}
using EarlySize = std::size_t; //! error
