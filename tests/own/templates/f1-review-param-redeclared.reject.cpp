// ζ f1 (#88), from the step's review: a template parameter's name is not redeclared in its
// scope ([temp.local]/6).
template<class T> int f() { int T = 0; return T; } //! error
int main() { return 0; }
