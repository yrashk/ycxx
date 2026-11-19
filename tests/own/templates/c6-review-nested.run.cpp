// ζ c6 (#88), from the step's review: a nested requirement is checked where it is written, with
// the requires-expression's parameters in scope; one that names no template parameter is checked
// at once, and its || and && short-circuit ([expr.prim.req.nested], [temp.constr.op]).
// EXIT: 0
template<class T> concept C = requires(T t) { requires sizeof(t) > 2; };
template<class T> concept D = requires { requires sizeof(int) > 2; };
template<class T> int f(T) requires requires { requires true; } { return 1; }
static_assert(C<int> && !C<char> && D<char>);
static_assert(requires { requires true || false; } && !requires { requires false; });
int main() { return f(1) == 1 ? 0 : 1; }
