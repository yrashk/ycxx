// ζ c6 (#88): templates that differ only in their constraints are distinct overloads, not
// redeclarations ([temp.over.link]/7); a concept-id is subsumed through its definition, so the
// conjunction is preferred ([temp.constr.normal]/1.4, [temp.constr.order]/1).
// EXIT: 0
template<class T> constexpr bool integral = false;
template<> constexpr bool integral<char> = true;
template<> constexpr bool integral<int> = true;
template<> constexpr bool integral<unsigned> = true;
template<class T> constexpr bool pointer = false;
template<class T> constexpr bool pointer<T*> = true;
template<class T> concept Int = integral<T>;
template<class T> concept Signed = Int<T> && (T(-1) < T(0));
template<class T> int f(T) requires Int<T> { return 1; }
template<class T> int f(T) requires Signed<T> { return 2; }
template<class T> int f(T) requires (!Int<T>) { return 3; }
template<class T> int g(T) requires Int<T> || pointer<T> { return 1; }
template<class T> int g(T) requires Int<T> { return 2; }
int main() {
  int x = 0;
  return f(1u) == 1 && f(1) == 2 && f(&x) == 3 && g(&x) == 1 && g(1) == 2 ? 0 : 1;
}
