// ι (#300), from the review:  default arguments only in a function declaration's own parameters ([dcl.fct.default]/3).
int (*p)(int = 3); //! error
typedef int F(int = 1); //! error
int f(int (*q)(int = 1)); //! error
int h(int k) { return k; }
int (*g(int x = 4))(int) { return x == 4 ? h : nullptr; }  // g's own: allowed
int main() { return g()(0); }
