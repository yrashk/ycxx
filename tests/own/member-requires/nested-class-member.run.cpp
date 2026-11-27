// #1417 [dcl.decl.general], [temp.mem.class], [over.match.viable]: a member function of a
// nested class of a class template is templated, so it may carry a trailing requires-clause
// that names the enclosing template's parameter.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Outer {
  struct Inner {
    int f() const requires Small<T> { return 1; }
    int f() const requires (!Small<T>) { return 2; }
    Inner() requires Small<T> : made(10) {}
    Inner() requires (!Small<T>) : made(20) {}
    int made;
  };
};

int main() {
  Outer<char>::Inner a;
  Outer<int>::Inner b;
  if (a.f() != 1 || a.made != 10) return 1;
  if (b.f() != 2 || b.made != 20) return 2;
  return 0;
}
