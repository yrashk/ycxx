// #209 [basic.lookup.general], [namespace.qual], [temp.names].
namespace predicates {
template<class T> concept Fits = sizeof(T) > 0;
}

namespace settings {
constexpr bool Fits = false;
}

namespace combined {
using namespace predicates;
using namespace settings;
}

template<bool> struct Bool {};
Bool<combined::Fits<int>> ambiguous; //! error
