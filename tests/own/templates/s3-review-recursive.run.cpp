// ζ s3 (#88), from the step's review: in an explicit or partial specialization's initializer the
// template's name still names the template ([temp.expl.spec], [temp.spec.partial]), and an
// explicit specialization's initializer may use a constant computed by a function template
// ([temp.inst]/7).
// EXIT: 0
template<class T> constexpr T f(T x) { return x; }
template<class T> constexpr int v = 1;
template<> constexpr int v<int> = v<long> + 1;
template<class T> constexpr int v<T*> = v<T> + 10;
static_assert(v<int> == 2 && v<int*> == 12 && v<char**> == 21);
constexpr int a = f(1);
template<> constexpr int v<short> = a;
static_assert(v<short> == 1);
int main() { return v<int> + v<int*> == 14 ? 0 : 1; }
