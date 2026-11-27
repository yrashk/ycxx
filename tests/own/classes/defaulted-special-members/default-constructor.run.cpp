// #157 [class.default.ctor], [dcl.init.general]: a defaulted default constructor, which is not
// user-provided: value-initialization zero-initializes first.
// EXIT: 0
void* operator new(decltype(sizeof(0)), void* p) noexcept { return p; }

int made = 0;
struct Part {
  int v;
  Part() : v(5) { ++made; }
};

struct Plain {
  int a;
  int b[2];
  Plain() = default;
  Plain(int x) : a(x), b{x, x} {}
};
struct Initialized {
  int a = 3;
  int b;
  Part p;
  Initialized() = default;
};
struct Holder {
  Plain p;
  Part q[2];
  Holder() = default;
};
struct Derived : Initialized {
  int c;
  Derived() = default;
};
struct Compile {
  int a = 2;
  int b = 3;
  constexpr Compile() = default;
};
constexpr Compile compiled{};
static_assert(compiled.a == 2 && compiled.b == 3);
struct Scalars {
  int a;
  long b;
  Scalars() = default;
  constexpr Scalars(int x) : a(x), b(x) {}
};
constexpr Scalars zero{};
static_assert(zero.a == 0 && zero.b == 0);

Plain global;
Initialized initialized;

int main() {
  if (global.a != 0 || global.b[1] != 0) return 1;
  if (initialized.a != 3 || initialized.p.v != 5) return 2;
  Plain p = Plain();
  Plain q{};
  if (p.a != 0 || p.b[0] != 0 || q.b[1] != 0) return 3;
  made = 0;
  Initialized i{};
  if (i.a != 3 || i.b != 0 || i.p.v != 5 || made != 1) return 4;
  made = 0;
  Holder h;
  if (h.q[0].v != 5 || h.q[1].v != 5 || made != 2) return 5;
  Derived d = Derived();
  if (d.a != 3 || d.c != 0 || d.b != 0) return 6;
  long raw[2] = {-1, -1};
  Plain* placed = new (raw) Plain();
  if (placed->a != 0 || placed->b[1] != 0) return 7;
  Plain some[2];
  some[1] = Plain(8);
  return some[1].b[1] == 8 ? 0 : 8;
}
