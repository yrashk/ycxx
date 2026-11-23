// #209 [basic.lookup.general], [basic.lookup.qual], [namespace.qual].
namespace first::Inner { template<class> concept Fits = true; }
namespace second::Inner { template<class T> concept Fits = sizeof(T) > 0; }

namespace combined {
using namespace first;
using namespace second;
}

template<bool> struct Bool {};
Bool<combined::Inner::Fits<int>> ambiguous; //! error
