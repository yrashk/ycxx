// ζ s3 (#88), from the step's review: a constant evaluation that instantiated a specialization
// is a use before its explicit specialization ([temp.expl.spec]).
template<class T> constexpr T f(T x) { return x; }
template<class T> constexpr T g(T x) { return f(x); }
static_assert(g(1) == 1);
template<> constexpr int f<int>(int x) { return x + 1; } //! error
template<class T> constexpr int r = 0;
static_assert(r<int> == 0);
template<> constexpr int r<int> = 1; //! error
int main() { return 0; }
