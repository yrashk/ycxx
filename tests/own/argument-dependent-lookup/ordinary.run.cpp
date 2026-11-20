// [basic.lookup.argdep], [over.call.func] (#165)
// EXIT: 0
namespace keys {
struct Key {};

int discover(Key) { return 11; }
int rank(Key, int) { return 21; }
int ignore_directive(Key, long) { return 31; }
int declared_before(Key, long) { return 41; }
int ordinary_directive(Key, long) { return 52; }
}

namespace other {
int ignore_directive(keys::Key, int) { return 39; }
int ordinary_directive(keys::Key, int) { return 51; }
int via_declaration(keys::Key) { return 61; }
}

namespace keys {
using namespace other;
using other::via_declaration;
}

int rank(keys::Key, long) { return 22; }

int call_before_new_overload(keys::Key key) {
    return declared_before(key, 0);
}

namespace keys {
int declared_before(Key, int) { return 49; }
}

namespace values {
struct Value {};

int accept(keys::Key, Value) { return 71; }
}

int main() {
    keys::Key key{};
    if (discover(key) != 11) return 1;
    if (rank(key, 0) != 21) return 2;
    if (rank(key, 0L) != 22) return 3;
    if (ignore_directive(key, 0) != 31) return 4;
    if (call_before_new_overload(key) != 41) return 5;
    if (declared_before(key, 0) != 49) return 6;
    {
        using namespace other;
        if (ordinary_directive(key, 0) != 51) return 7;
    }
    if (via_declaration(key) != 61) return 8;
    if (accept(key, values::Value{}) != 71) return 9;
    return 0;
}
