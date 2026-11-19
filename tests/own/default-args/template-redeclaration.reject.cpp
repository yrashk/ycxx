// ι (#300), from the review:  a later declaration of a function template adds no default arguments ([dcl.fct.default]/4).
template<class T> int f(T, int);
template<class T> int f(T, int = 2); //! error
template<class T> int f(T a, int b) { return a + b; }
int main() { return f(1, 2) == 3 ? 0 : 1; }
