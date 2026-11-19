// ι (#300), from the review:  an explicit specialization has no default arguments ([dcl.fct.default]/4).
template<class T> int f(T a, int b);
template<> int f<int>(int a, int b = 2) { return 0; } //! error
int main() { return 0; }
