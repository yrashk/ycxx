// #356: a dependent default argument of class type creates a temporary per call that uses it,
// destroyed at the end of the full-expression containing the call ([class.temporary],
// [dcl.fct.default], [temp.inst]).
// EXIT: 0
int live = 0, made = 0;

struct Tracked {
  int v;
  Tracked(int x) : v(x) { ++live; ++made; }
  Tracked(const Tracked& o) : v(o.v) { ++live; ++made; }
  ~Tracked() { --live; }
};

template<class T>
struct User {
  int peek(const T& t = T(sizeof(T))) { return live * 100 + t.v; }
  User(const T& t = T(7)) : v(t.v) {}
  int v;
};

int main() {
  User<Tracked> u;
  if (u.v != 7 || live != 0 || made != 1) return 1;
  int s = static_cast<int>(sizeof(Tracked));
  if (u.peek() != 100 + s) return 2;
  if (live != 0 || made != 2) return 3;
  int during = (u.peek(), live);
  if (during != 1 || live != 0) return 4;
  Tracked mine(5);
  if (u.peek(mine) != 105 || made != 4) return 5;
  if (live != 1) return 6;
  return 0;
}
