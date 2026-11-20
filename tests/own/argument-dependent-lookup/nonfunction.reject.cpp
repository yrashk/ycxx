// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
int inspect(Key);
}

int call(keys::Key key) {
    int (*inspect)(int) = nullptr;
    return inspect(key); //! error
}
