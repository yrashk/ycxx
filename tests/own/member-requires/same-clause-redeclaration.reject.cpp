// #1417 [basic.scope.scope], [class.mem.general], [temp.over.link]: two member declarations
// with the same name, parameter types and equivalent trailing requires-clauses correspond, so
// declaring both in the class is a redeclaration of a member, which is ill-formed.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct S {
  int f() requires Small<T>;
  int f() requires (!Small<T>);
  int f() requires Small<T>; //! error
};
