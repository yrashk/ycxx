// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
int inspect(Key);
}

namespace explicit_lookup {
int inspect(int);
}

int call(keys::Key key) {
    return explicit_lookup::inspect(key); //! error
}
