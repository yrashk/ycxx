// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace keys {
struct Key {};
}

namespace library {
int inspect(keys::Key, long) { return 41; }
}

namespace keys {
using library::inspect;
}

namespace library {
int inspect(keys::Key, int) { return 42; }
}

int main() {
    return inspect(keys::Key{}, 0) == 41 ? 0 : 1;
}
