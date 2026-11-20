// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace keys {
struct Key {};

int inspect(Key, int) { return 21; }
}

auto inspect(keys::Key, long);

int main() {
    return inspect(keys::Key{}, 0) == 21 ? 0 : 1;
}
