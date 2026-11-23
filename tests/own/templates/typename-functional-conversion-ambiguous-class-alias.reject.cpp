// #917 [basic.lookup.general], [namespace.qual], [expr.type.conv].
namespace first { struct Reading { int value; Reading(int, int); }; }
namespace second {
struct Alternate { int value; Alternate(int, int); };
using Reading = Alternate;
}

namespace combined {
using namespace first;
using namespace second;
}

int read() {
  return typename combined::Reading(1, 2).value; //! error
}
