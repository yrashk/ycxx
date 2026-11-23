// #209 [basic.lookup.qual], [namespace.qual].
namespace origin::Inner { template<class> concept Fits = true; }
namespace first { using namespace origin; }
namespace second { using namespace origin; }

namespace combined {
using namespace first;
using namespace second;
}

template<bool> struct Bool {};
static_assert(combined::Inner::Fits<int>);
static_assert(__is_same(Bool<combined::Inner::Fits<int>>, Bool<true>));
