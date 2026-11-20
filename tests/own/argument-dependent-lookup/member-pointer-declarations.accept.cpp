// [basic.lookup.argdep], [dcl.mptr] (#165)
namespace values {
struct Value {};
}

namespace owners {
struct Owner;

int via_owner(values::Value Owner::*);
}

namespace values {
int via_value(Value owners::Owner::*);
}

auto detect_owner(values::Value owners::Owner::* member) -> decltype(via_owner(member));
auto detect_value(values::Value owners::Owner::* member) -> decltype(via_value(member));
