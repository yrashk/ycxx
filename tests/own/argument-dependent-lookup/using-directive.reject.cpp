// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
}

namespace other {
int inspect(keys::Key);
}

namespace keys {
using namespace other;
}

int call(keys::Key key) {
    return inspect(key); //! error
}
