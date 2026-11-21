// #218: A trivially copyable class has at least one eligible copy or move operation ([class.prop]/1).
// XFAIL: clang,gcc Both peer builtins return true for this all-deleted class; #218 records the draft result.
struct Value { Value() = default; Value(const Value&) = delete; Value(Value&&) = delete; Value& operator=(const Value&) = delete; Value& operator=(Value&&) = delete; };
static_assert(!__is_trivially_copyable(Value));
