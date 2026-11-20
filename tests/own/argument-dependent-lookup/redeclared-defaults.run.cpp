// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace keys {
struct Key {};

int inspect(Key, int first, int second = 8);
}

int early(keys::Key key) {
    return inspect(key, 2);
}

namespace keys {
int inspect(Key, int first = 7, int second);
}

int later(keys::Key key) {
    return inspect(key);
}

namespace keys {
int inspect(Key, int first, int second) { return 10 * first + second; }
}

int main() {
    if (early(keys::Key{}) != 28) return 1;
    if (later(keys::Key{}) != 78) return 2;
    return 0;
}
