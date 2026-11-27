// #1417 [class.mem.general], [over.match.viable], [over.match.best.general]: a static member
// function may carry a trailing requires-clause; calls through the class name and through an
// object both consider only satisfied candidates and prefer the more constrained one.
template<class T> concept HasSize4 = sizeof(T) == 4;
template<class T> concept HasSize4AndSigned = HasSize4<T> && (T(-1) < T(0));

template<class T>
struct Meta {
  static int id() { return 10; }
  static int id() requires HasSize4<T> { return 20; }
  static int id() requires HasSize4AndSigned<T> { return 30; }
};

int main() {
  if (Meta<char>::id() != 10) return 1;
  if (Meta<unsigned>::id() != 20) return 2;
  if (Meta<int>::id() != 30) return 3;
  Meta<int> m;
  if (m.id() != 30) return 4;
  return 0;
}
