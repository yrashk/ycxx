// #1417 [class.copy.ctor], [special], [over.match.viable], [class.prop]: a move constructor
// family: a constrained `= default` one and a constrained user-provided one. When neither is
// satisfied there is still no implicit move constructor (both are user-declared), so an
// rvalue binds to the copy constructor.
int moves = 0;
int copies = 0;

template<class T> concept Small = sizeof(T) <= 2;
template<class T> concept Medium = sizeof(T) > 2 && sizeof(T) <= 4;

template<class T>
struct M {
  T value;
  M(T v) : value(v) {}
  M(const M& o) : value(o.value) { ++copies; }
  M(M&&) requires Small<T> = default;
  M(M&& o) requires Medium<T> : value(o.value) { ++moves; }
};

template<class T> T&& move(T& t) { return static_cast<T&&>(t); }


int main() {
  M<char> a('x');
  M<char> b(move(a));
  if (moves != 0 || copies != 0 || b.value != 'x') return 1;

  M<int> c(4);
  M<int> d(move(c));
  if (moves != 1 || copies != 0 || d.value != 4) return 2;

  M<long long> e(8);
  M<long long> f(move(e));   // both move constructors unsatisfied: the copy constructor binds
  if (moves != 1 || copies != 1 || f.value != 8) return 3;
  return 0;
}
