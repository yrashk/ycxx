// #1418 [class.mem.general], [dcl.init.aggr], [basic.start.static], [stmt.dcl]: folded array
// default member initializers in objects of static storage duration, static locals, const
// objects, members of members, arrays of such classes, and arrays of addresses.
// EXIT: 0
int global = 4;
const char text[] = "ab";

template <int N> struct Wd { unsigned long long w[N] = {}; };
template <int N> struct Partial { int w[N] = {1, 2}; };
template <int N> struct Holder { Partial<N> inner; int tail = 9; };  // a member of a member
template <int N> struct Addresses { int* p[N] = {&global}; const char* s[N] = {text, text + 1}; };
struct Plain { short w[2][2] = {{1}, {2, 3}}; };

Wd<3> namespace_object;
Partial<4> namespace_partial;
const Partial<3> const_namespace;

int run() {
  static Partial<3> local_static;
  if (local_static.w[0] != 1 || local_static.w[2] != 0) return 1;
  local_static.w[2] = 5;  // a static local is initialized once
  return 0;
}

int main() {
  for (unsigned long long v : namespace_object.w)
    if (v != 0) return 10;
  if (namespace_partial.w[1] != 2 || namespace_partial.w[3] != 0) return 11;
  if (const_namespace.w[0] != 1 || const_namespace.w[2] != 0) return 12;
  if (run() || run() != 1) return 13;

  const Partial<2> c;
  if (c.w[0] != 1 || c.w[1] != 2) return 20;
  Holder<3> h;
  if (h.inner.w[0] != 1 || h.inner.w[1] != 2 || h.inner.w[2] != 0 || h.tail != 9) return 21;
  Holder<3> hv{};
  if (hv.inner.w[1] != 2 || hv.inner.w[2] != 0 || hv.tail != 9) return 22;
  Partial<2> many[3];
  for (const Partial<2>& e : many)
    if (e.w[0] != 1 || e.w[1] != 2) return 23;
  Wd<2> list[2] = {};
  if (list[1].w[1] != 0) return 24;

  Addresses<2> a;
  if (a.p[0] != &global || a.p[1] != nullptr || *a.p[0] != 4) return 30;
  if (a.s[0] != text || *a.s[1] != 'b') return 31;
  Plain plain;
  if (plain.w[0][0] != 1 || plain.w[0][1] != 0 || plain.w[1][0] != 2 || plain.w[1][1] != 3) return 32;
  return 0;
}
