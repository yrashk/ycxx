// #1418 [class.mem.general], [dcl.init.aggr], [class.base.init]/9, [temp.inst]/3: a default member
// initializer of an array member whose bound or element type depends on a template parameter
// initializes the member wherever a specialization is default- or aggregate-initialized. An empty
// list zero-initializes every element and a partial list zero-initializes the rest. Each object
// is created in storage filled with 0xa5 bytes, so an element left uninitialized would show.
// EXIT: 0

using Size = decltype(sizeof(0));
[[gnu::visibility("hidden")]] constexpr void* operator new(Size, void* p) noexcept { return p; }

unsigned long long storage[32];
constexpr unsigned long long dirty_word = 0xa5a5a5a5a5a5a5a5ull;
void* dirty() {
  for (unsigned long long& word : storage) word = dirty_word;
  return storage;
}

template <int N> struct Wd { unsigned long long w[N] = {}; };       // `= {}`
template <int N> struct Braced { int w[N]{}; };                      // `{}`
template <int N> struct Partial { int w[N] = {1, 2}; };              // fewer clauses than elements
template <class T, int N> struct Typed { T w[N] = {}; };             // dependent element type
template <int N> struct Grid { short w[N][2] = {{3}, {4, 5}}; };     // nested arrays
template <int N> struct Mixed { int a = N; long w[N] = {}; int b; }; // with other members
template <int N> struct Bare { unsigned long long w[N]; };           // the control: no initializer
struct Plain { unsigned long long w[3] = {}; };                      // no template at all
enum class Color : unsigned char { red = 0, green = 7 };

// The issue's example: a specialization's default initialization in a function template.
template <class T> constexpr Wd<2> make(T) {
  Wd<2> r;
  r.w[0] = 1;
  return r;
}

template <int N> bool all_zero(const unsigned long long (&w)[N]) {
  for (int i = 0; i < N; ++i)
    if (w[i] != 0) return false;
  return true;
}

int default_initialized() {
  // Default-initialization leaves a member without an initializer alone: the storage is dirty.
  if (Bare<2>* bare = new (dirty()) Bare<2>; bare->w[1] != dirty_word) return 1;
  if (!all_zero((new (dirty()) Wd<4>)->w)) return 2;
  if (!all_zero((new (dirty()) Plain)->w)) return 3;
  Braced<3>* b = new (dirty()) Braced<3>;
  if (b->w[0] != 0 || b->w[1] != 0 || b->w[2] != 0) return 4;
  Partial<4>* c = new (dirty()) Partial<4>;
  if (c->w[0] != 1 || c->w[1] != 2 || c->w[2] != 0 || c->w[3] != 0) return 5;
  return 0;
}

int typed() {
  Typed<double, 3>* d = new (dirty()) Typed<double, 3>;
  if (d->w[0] != 0.0 || d->w[1] != 0.0 || d->w[2] != 0.0) return 10;
  Typed<int*, 2>* p = new (dirty()) Typed<int*, 2>;
  if (p->w[0] != nullptr || p->w[1] != nullptr) return 11;
  Typed<Color, 2>* e = new (dirty()) Typed<Color, 2>;
  if (e->w[0] != Color::red || e->w[1] != Color::red) return 12;
  Typed<char, 5>* s = new (dirty()) Typed<char, 5>;
  for (char ch : s->w)
    if (ch != 0) return 13;
  return 0;
}

int nested_and_mixed() {
  Grid<3>* g = new (dirty()) Grid<3>;
  if (g->w[0][0] != 3 || g->w[0][1] != 0 || g->w[1][0] != 4 || g->w[1][1] != 5 || g->w[2][0] != 0 ||
      g->w[2][1] != 0)
    return 20;
  Mixed<2>* m = new (dirty()) Mixed<2>{};
  if (m->a != 2 || m->w[0] != 0 || m->w[1] != 0 || m->b != 0) return 21;
  Mixed<3>* n = new (dirty()) Mixed<3>{9};  // the omitted array takes its default ([dcl.init.aggr]/5)
  if (n->a != 9 || n->w[0] != 0 || n->w[2] != 0 || n->b != 0) return 22;
  return 0;
}

int aggregate_initialized() {
  if (!all_zero((new (dirty()) Wd<3>{})->w)) return 30;
  Partial<3>* b = new (dirty()) Partial<3>{};
  if (b->w[0] != 1 || b->w[1] != 2 || b->w[2] != 0) return 31;
  Partial<3>* c = new (dirty()) Partial<3>{{7}};  // an explicit clause replaces the default
  if (c->w[0] != 7 || c->w[1] != 0 || c->w[2] != 0) return 32;
  if (Wd<2>{}.w[1] != 0 || Partial<2>{}.w[1] != 2) return 33;
  Partial<3> local = {};
  if (local.w[0] != 1 || local.w[2] != 0) return 34;
  return 0;
}

template <int N> unsigned long long in_template() {
  Wd<N>* object = new (dirty()) Wd<N>;
  unsigned long long sum = 0;
  for (unsigned long long v : object->w) sum += v;
  Wd<N> local;
  for (unsigned long long v : local.w) sum += v;
  return sum;
}

int main() {
  if (int r = default_initialized()) return r;
  if (int r = typed()) return r;
  if (int r = nested_and_mixed()) return r;
  if (int r = aggregate_initialized()) return r;
  if (in_template<5>() != 0) return 40;
  const Wd<2> made = make(1.0);
  if (made.w[0] != 1 || made.w[1] != 0) return 41;
  return 0;
}
