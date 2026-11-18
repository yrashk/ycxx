// ζ c2 (#88): a template-id names a type wherever a type can appear: a variable, a parameter by
// reference and by pointer, a pointer variable, a reference variable, and the return type (a
// reference or a pointer) of ordinary functions ([temp.names], [temp.class], [dcl.ref],
// [dcl.ptr]).
// EXIT: 0
template<class T>
struct Box {
  T v;
};

int read(const Box<int>& b) { return b.v; }
void write(Box<int>& b, int x) { b.v = x; }
void bump(Box<long>* p) { p->v += 1; }
Box<int>& pick(Box<int>& a, Box<int>& b, bool first) { return first ? a : b; }
Box<char>* address(Box<char>& c) { return &c; }

int main() {
  Box<int> a{1};
  Box<int> b{2};
  if (read(a) != 1) return 1;
  write(b, 20);
  if (b.v != 20) return 2;
  if (read(pick(a, b, false)) != 20) return 3;
  pick(a, b, true).v = 10;
  if (a.v != 10) return 4;
  Box<long> l{100L};
  bump(&l);
  if (l.v != 101L) return 5;
  Box<long>* lp = &l;
  bump(lp);
  if (lp->v != 102L) return 6;
  Box<char> c{'m'};
  if (address(c)->v != 'm') return 7;
  Box<int>& r = a;
  r.v = 3;
  if (a.v != 3) return 8;
  return 0;
}
