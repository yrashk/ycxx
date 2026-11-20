// [basic.lookup.argdep] (#165)
// EXIT: 0
namespace root {
struct Base {};

int via_base(const Base&) { return 81; }
}

namespace derived {
struct Type : root::Base {};
}

namespace api {
struct Outer {};

inline namespace v1 {
struct Inner {};

int from_outer(Outer) { return 82; }
}

int from_inline(v1::Inner) { return 83; }
}

int main() {
    if (via_base(derived::Type{}) != 81) return 1;
    if (from_outer(api::Outer{}) != 82) return 2;
    if (from_inline(api::v1::Inner{}) != 83) return 3;
    return 0;
}
