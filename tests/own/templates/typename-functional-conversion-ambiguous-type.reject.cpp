// #917 [expr.type.conv], [temp.res.general], [namespace.qual].
namespace first { struct Reading { int value; Reading(int, int); }; }
namespace second { struct Reading { int value; Reading(int, int); }; }

namespace combined {
using namespace first;
using namespace second;
}

int read() {
  return typename combined::Reading(1, 2).value; //! error
}
