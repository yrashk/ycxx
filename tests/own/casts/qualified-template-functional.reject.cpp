// The qualified lookup set can contain a type template and a non-type template.
namespace Types { template<class T> struct Tag {}; }
namespace Functions { template<class T> int Tag(); }
namespace Variables { template<class T> int Tag = 0; }

namespace MixedFunctions { using namespace Types; using namespace Functions; }
namespace MixedVariables { using namespace Types; using namespace Variables; }

auto mixed_functions = MixedFunctions::Tag<int>();  //! error
auto mixed_variables = MixedVariables::Tag<int>{};    //! error
