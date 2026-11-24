// [expr.new], [expr.delete], [temp.res]: new- and delete-expressions whose type or initializer
// depends on a template parameter, analysed when the template is instantiated.
// PEER: class-allocation-provider.cpp
using Size = decltype(sizeof(0));
int allocation_count() noexcept;
Size allocation_size(int) noexcept;
void* allocation_address(int) noexcept;
int release_count() noexcept;
Size release_size(int) noexcept;
void* release_address(int) noexcept;

static int destroyed;

struct S {
  int v;
  S(int x) : v(x) {}
  ~S() { destroyed += v; }
};
struct Point {
  int x, y;
};

template <class T> T* make() { return new T(); }
template <class T> T* make_default() { return new T; }
template <class T, class A> T* make_from(A a) { return new T(a); }
template <class T, class A, class B> T* make_list(A a, B b) { return new T{a, b}; }
template <class A> S* make_s(A a) { return new S(a); }  // a non-dependent type, dependent argument
template <class T> void destroy(T* p) { delete p; }
template <class T, class... A> T* make_pack(A... a) { return new T(a...); }

template <class T> struct Box {
  T* value;
  Box(int x) : value(new T(x)) {}
  ~Box() { delete value; }
};

int main() {
  Point* p = make<Point>();
  if (p->x != 0 || p->y != 0 || allocation_size(0) != sizeof(Point)) return 1;
  int* i = make<int>();
  if (*i != 0 || allocation_size(1) != sizeof(int)) return 2;
  S* s = make_from<S>(5);
  S* t = make_s(6);
  Point* l = make_list<Point>(3, 4);
  Point* d = make_default<Point>();
  d->x = 1;
  if (s->v != 5 || t->v != 6 || l->x != 3 || l->y != 4 || d->x != 1 || allocation_count() != 6) return 3;
  destroy(s);
  destroy(t);
  destroy(l);
  destroy(d);
  destroy(p);
  destroy(i);
  if (destroyed != 11 || release_count() != 6 || release_address(0) != allocation_address(2) ||
      release_size(0) != sizeof(S) || release_size(2) != sizeof(Point))
    return 4;
  Point* q = make_pack<Point>(8, 9);  // a parenthesized aggregate from a pack
  S* u = make_pack<S>(1);
  if (q->x != 8 || q->y != 9 || u->v != 1 || allocation_count() != 8) return 7;
  destroy(q);
  destroy(u);
  {
    Box<S> box(7);
    if (box.value->v != 7 || allocation_size(8) != sizeof(S)) return 5;
  }
  return destroyed == 19 && release_count() == 9 ? 0 : 6;
}
