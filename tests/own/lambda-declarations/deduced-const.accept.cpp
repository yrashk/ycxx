// A variable whose deduced type is const has internal linkage, which its initializer's lambda did
// not know when it was read (#338): the closure type is the same throughout.
namespace N {
template <class F>
const F cid(F f) { return f; }
}
decltype(auto) kept = N::cid([] { return 9; });
int call() { return kept(); }
