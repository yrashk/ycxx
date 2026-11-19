// ζ m9 (#88, #314), from the step's review: a constructor template never yields S(S)
// ([class.copy.ctor]/5), and S(S&) does not take a const S: no constructor copies c.
struct S { S(S&) {} template<class T> S(T) {} };
int main() { const S c(1); S d(c); } //! error
