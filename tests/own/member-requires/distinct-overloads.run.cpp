// #1417 [basic.scope.scope], [over.load], [class.mem.general]: member function declarations
// that differ only in their trailing requires-clauses do not correspond, so they declare
// distinct overloads in the same class; each is defined and each is reachable.
template<class T> concept One = sizeof(T) == 1;
template<class T> concept Two = sizeof(T) == 2;
template<class T> concept Four = sizeof(T) == 4;

template<class T>
struct Pick {
  int f(int x) requires One<T> { return x + 1; }
  int f(int x) requires Two<T> { return x + 2; }
  int f(int x) requires Four<T> { return x + 4; }
  int f(int x) requires (!One<T> && !Two<T> && !Four<T>) { return x; }
};

int main() {
  if (Pick<char>{}.f(100) != 101) return 1;
  if (Pick<short>{}.f(100) != 102) return 2;
  if (Pick<int>{}.f(100) != 104) return 3;
  if (Pick<double>{}.f(100) != 100) return 4;
  return 0;
}
