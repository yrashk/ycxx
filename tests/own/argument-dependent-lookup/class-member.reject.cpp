// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};
int inspect(Key);
}

struct Caller {
    static int inspect(int);

    int call(keys::Key key) {
        return inspect(key); //! error
    }
};
