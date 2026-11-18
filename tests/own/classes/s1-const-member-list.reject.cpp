// δ s1 (#86), from the step's review: `x = {…}` uses the deleted copy assignment of a class with a
// const member ([class.copy.assign]/7, [expr.assign]/9). (One line: compilers report the
// member's declaration too.)
struct C { const int x; }; void f(C& a) { a = {3}; } //! error
int main() { return 0; }
