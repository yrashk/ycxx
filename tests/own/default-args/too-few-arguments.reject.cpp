// ι (#300): a call may omit only arguments whose parameters have defaults; with none for `a`,
// f is not viable for a call without arguments ([expr.call]/5, [over.match.viable]/2).
int f(int a, int b = 2) { return a + b; }
int main() { return f(); } //! error
