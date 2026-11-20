// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
int inspect(Key, int);
}

int call(keys::Key key) {
    return inspect(key); //! error
}

namespace keys {
int inspect(Key, int = 7);
}
