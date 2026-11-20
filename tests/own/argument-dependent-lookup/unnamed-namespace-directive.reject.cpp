// [basic.lookup.argdep] (#165)
namespace keys {
struct Key {};

namespace {
[[maybe_unused]] int inspect(Key) { return 51; }
}
}

int call(keys::Key key) {
    return inspect(key); //! error
}
