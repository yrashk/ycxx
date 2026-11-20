// [basic.lookup.argdep], [dcl.meaning.general] (#165)
namespace keys {
struct Key {
    static int inspect(const Key*);
};

int Key::inspect(const Key*) { return 11; }
}

int qualified_call(const keys::Key* key) {
    return keys::Key::inspect(key);
}

int unqualified_call(const keys::Key* key) {
    return inspect(key); //! error
}
