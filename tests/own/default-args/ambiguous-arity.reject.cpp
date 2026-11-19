// ι (#300): f(int) and f(int, int = 0) both take one argument with the same conversion; the
// defaulted parameter is not ranked, so the call is ambiguous ([over.match.viable]/2,
// [over.match.best]).
int f(int) { return 1; }
int f(int, int = 0) { return 2; }
int main() { return f(1); } //! error
