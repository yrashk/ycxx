// #209 [basic.lookup.unqual], [basic.lookup.qual], [basic.lookup.general].
namespace first::Inner { template<class> concept Fits = true; }
namespace second::Inner { template<class T> concept Fits = sizeof(T) > 0; }

using namespace first;
using namespace second;

static_assert(Inner::Fits<int>); //! error
