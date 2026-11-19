// ι (#300): with no arguments, neither of two functions whose only parameter is defaulted is
// better than the other: the call is ambiguous ([over.match.viable]/2, [over.match.best]).
int f(int a = 1) { return a; }
int f(long a = 2) { return static_cast<int>(a); }
int main() { return f(); } //! error
