template<class T> consteval int immediate() { return 7; }
template<class P> consteval P leak(P p = immediate<P>) { return p; }
auto use() { return leak<int(*)()>(); } //! error

using Pointer = int(*)();
consteval int target() { return 7; }
template<class T> consteval Pointer make_pointer() { return target; }
template<class T> consteval Pointer nested_leak(Pointer p = make_pointer<T>()) { return p; }
Pointer use_nested() { return nested_leak<int>(); } //! error
constexpr Pointer escaped = make_pointer<int>(); //! error
constinit Pointer non_constexpr_escape = make_pointer<int>(); //! error

int consume_dynamically(Pointer) { return 8; }
int dynamic_initializer = consume_dynamically(make_pointer<int>()); //! error
