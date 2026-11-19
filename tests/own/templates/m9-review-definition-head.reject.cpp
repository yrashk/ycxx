// ζ m9 (#88, #314), from the step's review: a member template's definition outside the class has its
// template-head ([temp.over.link]/6): S declares no `g` whose head takes a value.
struct S { template<class T> int g(); };
template<int N> int S::g() { return N; } //! error
int main() { return 0; }
