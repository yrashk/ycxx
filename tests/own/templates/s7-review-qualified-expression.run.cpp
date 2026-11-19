// ζ s7 (#88), from the step's review: X<args>::member names a value, not a type, in a condition
// and in parentheses ([temp.res.general]/4); and as a template argument or array bound.
// EXIT: 0
template<int N> struct A { static constexpr int v = N * 2; };
template<class T> struct Tr { static constexpr bool big = sizeof(T) > 2; static constexpr int n = sizeof(T); };
template<class T> int f(T) { return (Tr<T>::n) - 3; }
template<class T> int g(T) { return (Tr<T>::n) ? 7 : 8; }
template<class T> int h(T) { while (Tr<T>::big) return 1; return 0; }
template<class T> int k(T) { if constexpr (Tr<T>::big) return Tr<T>::n; else return -Tr<T>::n; }
int arr[Tr<int>::n];
int main() {
  using X = A<Tr<int>::n>;
  return f(1) == 1 && g(1) == 7 && h(1) == 1 && h('c') == 0 && k(1) == 4 && k('c') == -1 &&
         sizeof(arr) == 16 && X::v == 8 ? 0 : 1;
}
