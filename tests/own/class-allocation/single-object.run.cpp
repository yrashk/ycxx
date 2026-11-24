// [expr.new], [expr.delete]: single objects of class type through the usual global allocation
// and sized deallocation functions, with each form of initialization ([dcl.init.general]).
// PEER: class-allocation-provider.cpp
using Size = decltype(sizeof(0));
int allocation_count() noexcept;
Size allocation_size(int) noexcept;
void* allocation_address(int) noexcept;
int release_count() noexcept;
Size release_size(int) noexcept;
void* release_address(int) noexcept;
int unsized_releases() noexcept;

static int constructed, destroyed;

struct S {
  int v;
  S(int x) : v(x) { ++constructed; }
  ~S() { destroyed += v; }
};
struct Pair {
  int x, y;
  Pair() : x(1), y(2) {}
  Pair(int a, int b) : x(a), y(b) {}
};
struct Aggregate {
  int a;
  long b;
};
struct Defaults {
  int a = 7;
  int b;
};
struct Holder {
  Pair p;  // a class member: the implicit default constructor is not trivial
  int n;
};

// Each allocation is checked against the next one the provider recorded, in order.
static int next;
static bool allocated(const void* object, Size size) {
  int i = next++;
  return allocation_count() >= next && allocation_size(i) == size && allocation_address(i) == object;
}
static bool released(int count, const void* object, Size size) {
  int i = count - 1;
  return release_count() == count && release_address(i) == object && release_size(i) == size;
}

int main() {
  S* p = new S(3);  // direct-initialization by a constructor
  if (p->v != 3 || constructed != 1 || !allocated(p, sizeof(S))) return 1;
  delete p;
  if (destroyed != 3 || !released(1, allocation_address(0), sizeof(S))) return 2;

  S* q = new S{4};  // direct-list-initialization
  Pair* d = new Pair;  // default-initialization: the default constructor
  Pair* r = new Pair(8, 9);
  Pair* s = new Pair{10, 11};
  if (q->v != 4 || !allocated(q, sizeof(S)) || d->x != 1 || d->y != 2 || !allocated(d, sizeof(Pair)) ||
      r->x != 8 || r->y != 9 || !allocated(r, sizeof(Pair)) || s->x != 10 || s->y != 11 ||
      !allocated(s, sizeof(Pair)))
    return 3;

  Aggregate* a = new Aggregate{5, 6};  // aggregate initialization
  Aggregate* z = new Aggregate();      // value-initialization: zero
  Aggregate* c = new Aggregate(*a);    // the implicit copy constructor
  if (a->a != 5 || a->b != 6 || !allocated(a, sizeof(Aggregate)) || z->a != 0 || z->b != 0 ||
      !allocated(z, sizeof(Aggregate)) || c->a != 5 || c->b != 6 || !allocated(c, sizeof(Aggregate)))
    return 4;

  Defaults* m = new Defaults();  // value-initialization: zero, then the default member initializer
  Defaults* n = new Defaults{};
  Holder* h = new Holder();  // zero-initialized before its implicit default constructor runs
  if (m->a != 7 || m->b != 0 || !allocated(m, sizeof(Defaults)) || n->a != 7 || n->b != 0 ||
      !allocated(n, sizeof(Defaults)) || h->p.x != 1 || h->p.y != 2 || h->n != 0 || !allocated(h, sizeof(Holder)))
    return 5;
  const Pair* k = new const Pair;  // const: the user-provided default constructor initializes it
  if (k->x != 1 || !allocated(k, sizeof(Pair)) || allocation_count() != 12) return 6;

  delete q;
  if (destroyed != 7 || !released(2, q, sizeof(S))) return 7;
  delete d;
  delete r;
  delete s;
  delete a;
  delete z;
  delete c;
  delete m;
  delete n;
  delete h;
  delete k;  // through a pointer to const
  if (!released(12, k, sizeof(Pair))) return 8;

  S* none = nullptr;
  delete none;  // a null pointer: no destructor and no deallocation call
  if (destroyed != 7 || release_count() != 12 || unsized_releases() != 0) return 9;
  return constructed == 2 ? 0 : 10;
}
