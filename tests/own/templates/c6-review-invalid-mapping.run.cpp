// ζ c6 (#88), from the step's review: an atom whose mapping is invalid for the arguments is not
// satisfied, and a disjunction around it still may be ([temp.constr.atomic]/3, [temp.constr.op]).
// EXIT: 0
template<class T> concept C = sizeof(T) > 0;
template<class T> concept D = C<typename T::type>;
template<class T> concept E = C<typename T::type> || true;
struct S { using type = int; };
static_assert(!D<int> && D<S> && E<int>);
template<class T> requires D<T> int f(T) { return 1; }
template<class T> int f(T) { return 2; }
int main() { return f(1) == 2 ? 0 : 1; }
