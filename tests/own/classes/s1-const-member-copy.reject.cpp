// δ s1 (#86), from the step's review: the copy assignment of a class with a const member is
// deleted ([class.copy.assign]/7). (One line: compilers report the member's declaration too.)
struct C { const int x; }; void f(C& a, const C& b) { a = b; } //! error
int main() { return 0; }
