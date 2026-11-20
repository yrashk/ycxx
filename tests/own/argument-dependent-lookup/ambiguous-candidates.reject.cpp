// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
int inspect(Key);
}

int inspect(keys::Key);

int call(keys::Key key) {
    return inspect(key); //! error
}
