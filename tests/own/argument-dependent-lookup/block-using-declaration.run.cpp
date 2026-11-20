// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace keys {
struct Key {};

int inspect(Key, int) { return 11; }
}

namespace fallback {
int inspect(keys::Key, long) { return 12; }
}

int main() {
    using fallback::inspect;
    return inspect(keys::Key{}, 0) == 11 ? 0 : 1;
}
