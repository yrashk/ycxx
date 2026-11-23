// #917 [basic.lookup.general], [namespace.qual], [expr.type.conv].
namespace first {
template<class T> struct Type { T value; Type(T); };
}

namespace second {
template<class T> struct Type { T value; Type(T); };
}

namespace Import {
using namespace first;
using namespace second;
}

int read() {
  return typename Import::Type<int>(3).value; //! error
}
