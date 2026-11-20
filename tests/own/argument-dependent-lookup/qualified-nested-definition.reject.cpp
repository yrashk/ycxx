// [basic.lookup.argdep], [dcl.meaning.general] (#165)
namespace keys {
struct Key;

namespace nested {
int inspect(const Key*);
}

int nested::inspect(const Key*) { return 12; }
}

int qualified_call(const keys::Key* key) {
    return keys::nested::inspect(key);
}

int unqualified_call(const keys::Key* key) {
    return inspect(key); //! error
}
