// Both using directives make a distinct concept available to unqualified lookup.
namespace left { template<class T> concept Value = true; }
namespace right { template<class T> concept Value = true; }
using namespace left;
using namespace right;
Value auto value = 1; //! error
