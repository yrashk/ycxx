// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace keys {
struct Key {};

int inspect(Key, long) { return 31; }
}

template<class T>
int dispatch() {
    return inspect(keys::Key{}, 0);
}

namespace keys {
int inspect(Key, int) { return 32; }
}

int main() {
    return dispatch<int>() == 31 ? 0 : 1;
}
