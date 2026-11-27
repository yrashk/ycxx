// #1417 [class.mem.general], [dcl.decl.general], [class.base.init], [over.match.ctor],
// [over.match.viable]: constructors with trailing requires-clauses, a ctor-initializer and a
// compound-statement body; only the satisfied constructor initializes the object.
template<class T> concept Small = sizeof(T) <= 2;

template<class T>
struct Cell {
  int origin;
  int extra;
  Cell(int v) requires Small<T> : origin(1), extra(v) { extra += 10; }
  Cell(int v) requires (!Small<T>) : origin(2), extra(v) { extra *= 2; }
  Cell(int a, int b) requires Small<T> : origin(3), extra(a - b) {}
};

int main() {
  Cell<char> s(5);
  Cell<long long> l(5);
  Cell<short> t(9, 4);
  if (s.origin != 1 || s.extra != 15) return 1;
  if (l.origin != 2 || l.extra != 10) return 2;
  if (t.origin != 3 || t.extra != 5) return 3;
  return 0;
}
