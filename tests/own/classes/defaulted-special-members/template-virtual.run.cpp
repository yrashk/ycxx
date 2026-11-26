// #157, #1153 [class.copy.ctor], [class.virtual], [temp.inst]: defaulted members of class templates with virtual
// functions: a copy or move installs its own class's table and runs the subobjects' operations.
// EXIT: 0
int ends = 0;
struct N { int v; N(int x = 1) : v(x) {} N(const N& o) : v(o.v + 1) {} ~N() { ++ends; } };
template <class T> struct B { T t; B() = default; B(const B&) = default; virtual ~B() = default; virtual int f() const { return t.v; } };
template <class T> struct D : B<T> { T u; D() = default; D(const D&) = default; D(D&&) = default; int f() const override { return 100 * this->t.v + u.v; } };
int call(const B<N>& b) { return b.f(); }
int main() {
  { D<N> d; D<N> e(d); D<N> g(static_cast<D<N>&&>(d)); B<N> s(e); if (call(e) != 202 || call(g) != 202 || call(s) != 3) return 1; }
  return ends == 7 ? 0 : 3;
}
