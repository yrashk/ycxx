// #234 item 2: partial specializations matched and ordered by their constraints
// ([temp.spec.partial.match]/2, [temp.spec.partial.order], [temp.constr.order]). A partial
// specialization with the primary's own arguments, more constrained than it; two ordered by
// subsumption; the pack form libycxx's invoke_result has; a declaration before the definition; a
// variable template; and a more specialized argument list against a more constrained one.
// EXIT: 0
template<class T> concept Four = sizeof(T) == 4;
template<class T> concept FourAligned = Four<T> && alignof(T) == 4;
template<class T> concept Small = sizeof(T) < 4;

template<class T> struct R { static constexpr int v = 1; };
template<class T> requires Four<T> struct R<T> { static constexpr int v = 2; };
template<class T> requires FourAligned<T> struct R<T> { static constexpr int v = 3; };
struct alignas(1) Packed { char c[4]; };
static_assert(R<char>::v == 1 && R<int>::v == 3 && R<Packed>::v == 2);

template<class F, class... A> concept Unary = sizeof...(A) == 1;
template<class F, class... A> struct Result {};
template<class F, class... A> requires Unary<F, A...> struct Result<F, A...> { using type = int; };
template<class T> concept HasType = requires { typename T::type; };
static_assert(HasType<Result<int (*)(int), int>> && !HasType<Result<int (*)(int)>> && !HasType<Result<int, int, int>>);

template<class T> struct S;
template<class T> requires Small<T> struct S<T>;
template<class T> struct S { static constexpr int v = 10; };
template<class T> requires Small<T> struct S<T> { static constexpr int v = 20; };
static_assert(S<char>::v == 20 && S<long>::v == 10);

template<class T> constexpr int var = 1;
template<class T> requires Four<T> constexpr int var<T> = 4;
static_assert(var<char> == 1 && var<int> == 4);

template<class T> struct Q { static constexpr int v = 1; };
template<class T> struct Q<T*> { static constexpr int v = 2; };
template<class T> requires Four<T> struct Q<T> { static constexpr int v = 3; };
static_assert(Q<int*>::v == 2 && Q<int>::v == 3 && Q<char>::v == 1);

int main() { return R<int>::v + S<char>::v - 23; }
