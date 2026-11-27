// #356: a const R& parameter of a function template whose dependent default creates a
// temporary: the temporary lives until the end of the full-expression containing the call,
// then is destroyed, once per use of the default ([class.temporary], [dcl.fct.default],
// [temp.inst]).
// EXIT: 0
int live = 0, made = 0;

struct R {
  int v;
  R(int x) : v(x) { ++live; ++made; }
  R(const R& o) : v(o.v) { ++live; ++made; }
  ~R() { --live; }
};

template<class T>
int peek(const R& r = R(sizeof(T) * 10)) { return live * 1000 + r.v; }

template<class T>
const R& pass(const R& r = R(sizeof(T))) { return r; }

int main() {
  if (peek<char>() != 1010) return 1;
  if (live != 0 || made != 1) return 2;
  int during = (peek<short>(), live);
  if (during != 1 || live != 0 || made != 2) return 3;
  int v = pass<int>().v;
  if (v != static_cast<int>(sizeof(int)) || live != 0 || made != 3) return 4;
  R mine(7);
  if (peek<char>(mine) != 1007 || made != 4) return 5;
  int both = peek<char>() + peek<short>();
  if (both != 2000 + 10 + 10 * static_cast<int>(sizeof(short)) && both != 1000 + 2000 + 10 + 10 * static_cast<int>(sizeof(short)) - 1000) return 6;
  if (live != 1 || made != 6) return 7;
  return 0;
}
