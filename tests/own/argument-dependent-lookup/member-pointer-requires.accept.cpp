// [basic.lookup.argdep], [expr.prim.req], [dcl.mptr] (#165)
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

using MemberPointer = values::Value owners::Owner::*;

static_assert(requires(MemberPointer member) {
    via_owner(member);
    { via_value(member) };
});

template<class Pointer>
concept AssociatedCalls = requires(Pointer member) {
    via_owner(member);
    { via_value(member) };
};

static_assert(AssociatedCalls<MemberPointer>);
static_assert(!AssociatedCalls<int>);
