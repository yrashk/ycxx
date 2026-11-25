// #317, [expr.const], [temp.arg.nontype]: a runtime read cannot form a class argument.
struct Value { long num, den; bool ok; };
struct Runtime { static long n; };

template<Value> struct Box {};

// Share a diagnostic line for the runtime read and its constant-expression demand.
namespace d {
  template<class, class U> consteval Value make() {
    return {-3, U::n, true}; } } template<class T, class U> using M = Box<d::make<T, U>()>; //! error

using Demanded = M<int, Runtime>;
