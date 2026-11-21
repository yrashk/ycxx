// #218: The ultimate class element has a nontrivial destructor.
struct Value { ~Value() {} };
static_assert(!__is_trivially_copyable(Value[][3]));
