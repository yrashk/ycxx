namespace Scope {
consteval int pick(int n) { return n; }
inline namespace Inner { struct pick {}; }
}
template<int N, int A = Scope::pick(N)> struct Result; //! error
