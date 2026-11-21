template<class T> consteval int immediate() { return 7; }
template<class P> constexpr int sink(P p = immediate<P>) { return 8; }
static_assert(sink<int(*)()>() == 8);

using NestedPointer = int(*)();
consteval int nested_target() { return 7; }
consteval NestedPointer identity_pointer(NestedPointer p) { return p; }
constexpr int consume_pointer(NestedPointer) { return 8; }
static_assert(consume_pointer(identity_pointer(nested_target)) == 8);

constinit int required_constant = consume_pointer(identity_pointer(nested_target));
consteval NestedPointer make_pointer() { return nested_target; }
consteval int invoke_pointer(NestedPointer p = make_pointer()) { return p(); }
constexpr int consumed = invoke_pointer();
static_assert(consumed == 7);
