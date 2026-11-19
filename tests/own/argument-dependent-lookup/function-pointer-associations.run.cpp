// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace parameters {
struct Argument {};
}

namespace results {
struct Result {};
}

using Function = results::Result (*)(parameters::Argument);

namespace parameters {
int via_parameter(Function pointer) { return pointer ? 61 : -1; }
}

namespace results {
int via_result(Function pointer) { return pointer ? 62 : -1; }
}

results::Result transform(parameters::Argument) { return {}; }

int main() {
    Function pointer = &transform;
    if (via_parameter(pointer) != 61) return 1;
    if (via_result(pointer) != 62) return 2;
    return 0;
}
