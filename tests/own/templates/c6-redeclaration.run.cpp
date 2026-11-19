// ζ c6 (#88): a declaration and a definition with equivalent requires-clauses declare one template,
// whatever the names of their parameters ([temp.over.link]/5,7); the other is a distinct overload.
// EXIT: 0
template<class T> constexpr bool small = sizeof(T) <= 2;
template<class T> requires small<T> int f(T);
template<class T> int f(T) requires (!small<T>);
template<class U> requires small<U> int f(U) { return 1; }
template<class V> int f(V) requires (!small<V>) { return 2; }
int main() { return f((char)0) == 1 && f(0) == 2 ? 0 : 1; }
