// #711 [stmt.ambig], [basic.scope.block]/2: `N::Value<int>(value);` is a declaration of `value`,
// which redeclares the parameter in the function's outermost block. `(void)N::Value<int>(value);`
// is the explicit expression control, and a nested block may declare the name again.
namespace N {
template <class> using Value = int;
struct S { S(int) {} };
template <class T> struct traits { using value_type = T; };
}

void conversion(int value) { (void)N::Value<int>(value); }
void nested(int value) { { N::Value<int>(value); value = 1; } }
void redeclared(int value) { N::Value<int>(value); } //! error
void redeclared_class(int value) { N::S(value); } //! error
template <class T> void redeclared_dependent(T value) { N::Value<T>(value); } //! error
// Control: a member of a dependent class still needs `typename` ([temp.res.general]/5).
template <class T> int needs_typename(T x) { N::traits<T>::value_type v(x); return v; } //! error

int main() { conversion(1); nested(1); }
