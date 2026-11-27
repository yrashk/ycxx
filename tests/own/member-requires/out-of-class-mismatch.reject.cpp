// #1417 [temp.mem.func], [temp.over.link], [class.mem.general]: an out-of-class definition
// must match a member declaration including its trailing requires-clause; a definition whose
// clause is not equivalent to any declared one defines no member and is ill-formed.
template<class T> concept Small = sizeof(T) <= 2;
template<class T> concept Big = sizeof(T) > 2;

template<class T>
struct S {
  int f() requires Small<T>;
};

template<class T> int S<T>::f() requires Small<T> { return 1; }
template<class T> int S<T>::f() requires Big<T> { return 2; } //! error
