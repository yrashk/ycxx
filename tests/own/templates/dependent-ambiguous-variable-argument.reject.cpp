namespace A { template<class> inline constexpr bool Flag = false; }
namespace B { template<class> inline constexpr bool Flag = true; }
using namespace A;
using namespace B;
template<bool> struct Select {};
template<class T> using Choice = Select<Flag<T>>; //! error
