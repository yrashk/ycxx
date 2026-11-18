// ζ s3 (#88), from the step's review: an explicit specialization not declared constexpr is not,
// though its template is ([temp.expl.spec], [expr.const]).
template<class T> constexpr int f(T) { return 1; }
template<> int f<int>(int) { return 7; }
static_assert(f(1) == 7); //! error
int main() { return 0; }
