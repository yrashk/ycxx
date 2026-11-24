// [expr.new], [new.delete.placement]: placement new of class type constructs the object in the
// given storage, with each form of initialization, also in a template ([specialized.construct]).
using Size = decltype(sizeof(0));
constexpr void* operator new(Size, void* p) noexcept { return p; }

static int destroyed;
struct S {
  int v;
  S(int x) : v(x) {}
  ~S() { destroyed += v; }
};
struct Point {
  int x, y;
};
struct Defaults {
  int a = 7;
  int b;
};

template <class T, class... A> T* construct_at(T* p, A... a) { return ::new (static_cast<void*>(p)) T(a...); }

union Storage {
  long long align;
  unsigned char bytes[64];
};
static void fill(unsigned char* b) {
  for (int i = 0; i < 64; ++i) b[i] = 0xa5;
}

int main() {
  Storage storage;
  unsigned char* buffer = storage.bytes;
  fill(buffer);
  S* s = new (buffer) S(3);
  if (static_cast<void*>(s) != static_cast<void*>(buffer) || s->v != 3) return 1;
  s->~S();
  Point* p = ::new (buffer) Point{4, 5};
  if (p->x != 4 || p->y != 5) return 2;
  fill(buffer);
  Defaults* d = new (buffer) Defaults();  // value-initialization zero-fills b
  if (d->a != 7 || d->b != 0) return 3;
  fill(buffer);
  Point* z = construct_at(static_cast<Point*>(static_cast<void*>(buffer)));  // T(): zero
  if (z->x != 0 || z->y != 0) return 4;
  S* t = construct_at(static_cast<S*>(static_cast<void*>(buffer)), 6);
  if (t->v != 6) return 5;
  t->~S();
  return destroyed == 9 ? 0 : 6;
}
