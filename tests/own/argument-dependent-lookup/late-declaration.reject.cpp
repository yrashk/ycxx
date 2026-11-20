// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
}

int call(keys::Key key) {
    return inspect(key); //! error
}

namespace keys {
int inspect(Key);
}
