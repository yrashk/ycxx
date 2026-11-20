// [expr.call], [over.match.oper]: explicit calls use function lookup, without the built-in
// and rewritten candidates of operator expressions.
namespace N {
enum E { value };
bool operator==(E, E);
int operator-(E, int);
}

bool rewritten() { return operator!=(N::value, N::value); }  //! error
int builtin() { return operator+(N::value, 1); }  //! error
int qualified() { return ::operator-(N::value, 1); }  //! error

struct Callable { int operator()(int); };
int member_only(Callable& c) { return ::operator()(c, 1); }  //! error
