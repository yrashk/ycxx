// #157 [class.default.ctor], [class.copy.ctor], [class.virtual]: a defaulted constructor of a
// polymorphic class installs its own class's table. A copy does not take the source's dynamic type.
// EXIT: 0
int ends = 0;

struct M {
  int tag = 4;
  M() noexcept = default;
  M(const M&) noexcept = default;
  virtual ~M();
  virtual int kind() const noexcept { return tag; }
};
M::~M() { ++ends; }

struct D : M {
  int own;
  D() noexcept = default;
  D(const D&) noexcept = default;
  D(D&&) noexcept = default;
  int kind() const noexcept override { return 100 + tag + own; }
};

int kind_of(const M& m) noexcept { return m.kind(); }

int main() {
  {
    M a;
    M b(a);
    a.tag = 9;
    if (kind_of(a) != 9 || kind_of(b) != 4) return 1;
    D d = D();
    if (kind_of(d) != 104) return 2;
    d.own = 2;
    d.tag = 1;
    D e(d);
    D f(static_cast<D&&>(d));
    if (kind_of(e) != 103 || kind_of(f) != 103) return 3;
    M sliced(e);
    if (kind_of(sliced) != 1) return 4;
  }
  return ends == 6 ? 0 : 5;
}
