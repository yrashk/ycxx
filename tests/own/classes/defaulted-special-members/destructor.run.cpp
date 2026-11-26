// #157 [class.dtor]: a defaulted destructor destroys its members in reverse order, then its bases.
// EXIT: 0
int log = 0;
template <int N>
struct Ends {
  ~Ends() { log = log * 10 + N; }
};

struct Whole : Ends<1> {
  Ends<2> a;
  Ends<3> b[2];
  int scalar;
  ~Whole() = default;
};

template <class T, class U>
struct Two {
  T t;
  U u;
  constexpr ~Two() = default;
};

class Closed {
  Ends<7> e;
  ~Closed() = default;
public:
  static void destroy(Closed* c) { c->~Closed(); }
  static void use() { Closed c; }
};

int main() {
  { Whole w; }
  if (log != 3321) return 1;
  log = 0;
  { Two<Ends<4>, Two<int, Ends<5>>> t; }
  if (log != 54) return 2;
  log = 0;
  Closed::use();
  if (log != 7) return 3;
  log = 0;
  { Two<int, long> t{1, 2}; if (t.u != 2) return 4; }
  return log == 0 ? 0 : 5;
}
