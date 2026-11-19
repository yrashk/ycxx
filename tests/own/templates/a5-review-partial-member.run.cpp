// ζ a5 (#88), from the step's review: an alias template of a partial specialization's member type
// uses the partial specialization ([temp.alias], [temp.spec.partial]).
// EXIT: 0
template<class T> struct S { int tag() { return 0; } };
template<class T> struct S<T*> { int tag() { return 1; } };
template<class T> using SP = S<T*>;
template<class T> struct R { using type = int; };
template<class T> struct R<T*> { using type = char; };
template<class T> using RT = typename R<T>::type;
int main() { SP<int> s; RT<int*> c = 0; RT<int> i = 0; return s.tag() - 1 + (sizeof(c) == 1 && sizeof(i) == 4 ? 0 : 5); }
