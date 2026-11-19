// ι (#300), from the review:  a later declarator of the same declaration adds defaults ([dcl.fct.default]/4).
// EXIT: 0
int f(int a, int b = 2), f(int a = 1, int b);
int f(int a, int b) { return a * 10 + b; }
int main() { return f() == 12 ? 0 : 1; }
