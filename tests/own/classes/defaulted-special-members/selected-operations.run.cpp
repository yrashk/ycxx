// #157 [class.copy.ctor]/14, [class.copy.assign]/12, [class.mem.general]: the operation overload
// resolution selects for each array element, classes nested after the defaulted member, and unused
// definitions that would be ill-formed.
// EXIT: 0
struct Generic {
  int v = 1;
  Generic() {}
  Generic(const Generic&) = default;
  Generic(Generic&&) = default;
  template <class T> Generic(T&& o) : v(o.v + 50) {}
};
struct Mutable {
  Mutable() = default;
  Mutable(Mutable&) = default;
  Generic n[2];
};

struct Later {
  Later(const Later&) = default;
  Later& operator=(const Later&) = default;
  struct Nested { int v; };
  Nested n;
  Later(int x) : n{x} {}
};

struct Part {
  int v;
  Part& operator=(const Part& o) { v = o.v + 1; return *this; }
};
struct Arrays {
  char text[300];
  int grid[3][4];
  Part parts[2];
  Arrays& operator=(const Arrays&) = default;
  Arrays& operator=(Arrays&&) = default;
};

struct Incomplete;
template <class T>
struct Owner {
  T* p = nullptr;
  Owner() {}
  Owner(const Owner&) { (void)sizeof(T); }
  ~Owner() { (void)sizeof(T); }
};
// Nothing uses its destructor or its copy, so neither is defined.
struct Unused {
  Unused(const Unused&) = default;
  ~Unused() = default;
  Owner<Incomplete> o;
};

int ends = 0;
struct End { ~End() { ++ends; } };
struct Holder {
  End e;
  Holder() noexcept = default;
  ~Holder() = default;
};
struct Outer { Holder h; int x; };
void destroy_locals() noexcept {
  Holder i;
  Outer o;
  Holder pair[2];
}

namespace {
struct Local {
  Part p;
  Local& operator=(const Local&) = default;
};
}

int main() {
  Mutable a;
  Mutable b(a);
  if (b.n[0].v != 51 || b.n[1].v != 51) return 1;

  Later c(3), d(4);
  Later e(c);
  d = e;
  if (d.n.v != 3) return 2;

  Arrays f{}, g{};
  f.text[299] = 'x';
  f.grid[2][3] = 9;
  f.parts[1].v = 5;
  g = f;
  if (g.text[299] != 'x' || g.grid[2][3] != 9 || g.parts[1].v != 6) return 3;
  Arrays h{};
  h = static_cast<Arrays&&>(g);
  if (h.text[299] != 'x' || h.grid[2][3] != 9 || h.parts[1].v != 7) return 4;

  destroy_locals();
  delete new Holder;
  if (ends != 5) return 5;

  Local k{{1}}, l{{2}};
  k = l;
  return k.p.v == 3 ? 0 : 7;
}
