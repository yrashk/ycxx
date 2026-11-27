// #340: a class's static data member template in the requires-clauses of its own member function
// templates selects between overloads ([temp.mem], [temp.constr.constr], [over.match.best]).
// EXIT: 0
template <class A> struct Box {
  template <class T> static constexpr bool fits = sizeof(T) <= sizeof(A);
  template <class T> requires fits<T> static int put(T) { return 1; }
  template <class T> requires (!fits<T>) static int put(T) { return 2; }
};
struct Plain {
  template <class T> static constexpr bool small = sizeof(T) == 1;
  template <class T> requires small<T> int get(T) const { return 10; }
  template <class T> int get(T) const { return 20; }
};
int main() {
  Plain p;
  int r = Box<int>::put('a') + Box<char>::put(1L) + p.get('a') + p.get(1);
  // 1 + 2 + 10 + 20
  return r == 33 ? 0 : 1;
}
