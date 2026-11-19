// ι (#300), from the review:  a parenthesized callee and unevaluated calls use the default arguments too ([expr.call]).
// EXIT: 0
int f(int x = 1) { return x; }
int main() { decltype(f()) a = 2; unsigned long s = sizeof(f()); return (f)() == 1 && a == 2 && s == sizeof(int) ? 0 : 1; }
