// #209 [basic.lookup.general], [namespace.qual], [temp.names].
namespace first {
template<class T> concept Fits = sizeof(T) > 0;
}

namespace second {
template<class T> concept Fits = sizeof(T) == 1;
}

namespace combined {
using namespace first;
using namespace second;
}

template<bool> struct Bool {};
Bool<combined::Fits<int>> ambiguous; //! error
