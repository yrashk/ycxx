namespace M {
constexpr int x = 11;
constexpr int fn(int v) { return v + 1; }
template <typename T>
constexpr int tfn(T v) {
  return static_cast<int>(v) + 10;
}
}  // namespace M

namespace M2 {
constexpr int fn(double v) { return static_cast<int>(v) + 100; }
}  // namespace M2

constexpr int test_block_and_local_class() {
  struct x {
    int a;
    struct Inner {
      int v;
    };
  };
  using M::x;
  struct x local_x{3};
  struct x* px = &local_x;
  x::Inner in{5};

  struct L {
    struct x member;
    x::Inner nested;
    constexpr int g() const { return x + member.a + nested.v; }
  };
  L l{{7}, {9}};
  return (x == 11 && px->a == 3 && in.v == 5 && l.g() == 27) ? 0 : 1;
}
static_assert(test_block_and_local_class() == 0);

constexpr int test_reverse_order() {
  using M::x;
  struct x {
    int a;
    struct Inner {
      int v;
    };
  };
  struct x local_x{4};
  struct L {
    struct x member;
    x::Inner nested;
    constexpr int g() const { return x + member.a + nested.v; }
  };
  L l{{2}, {6}};
  return (x == 11 && local_x.a == 4 && l.g() == 19) ? 0 : 2;
}
static_assert(test_reverse_order() == 0);

constexpr int test_overloads_and_templates() {
  struct fn {
    int a;
  };
  using M::fn;
  using M2::fn;
  struct tfn {
    int a;
  };
  using M::tfn;

  struct L {
    struct fn f;
    struct tfn t;
    constexpr int g() const { return fn(4) + fn(2.0) + tfn<int>(5) + f.a + t.a; }
  };
  L l{{1}, {2}};
  int direct = fn(3) + fn(1.0) + tfn<int>(6);
  return (direct == 4 + 101 + 16 && l.g() == 5 + 102 + 15 + 1 + 2) ? 0 : 3;
}
static_assert(test_overloads_and_templates() == 0);

constexpr int test_forward_declared_local_tag() {
  struct x;
  using M::x;
  struct x* p = nullptr;
  struct x {
    int a;
  };
  struct x val{8};
  p = &val;

  {
    using M::x;
    struct x;
    struct x* q = nullptr;
    struct x {
      int b;
    };
    struct x val2{9};
    q = &val2;
    if (x != 11 || q->b != 9) return 4;
  }

  return (x == 11 && p->a == 8) ? 0 : 4;
}
static_assert(test_forward_declared_local_tag() == 0);

int test_nested_blocks_and_lambdas() {
  struct x {
    int a;
    struct Inner {
      int v;
    };
  };
  int outer_ok = 0;
  {
    using M::x;
    struct x obj{6};
    x::Inner in{7};
    struct L {
      struct x member;
      x::Inner nested;
      constexpr int g() const { return x + member.a + nested.v; }
    };
    L l{{1}, {2}};
    auto lam = [&]() {
      struct x* ptr = &obj;
      return x + ptr->a + in.v;
    };
    auto glam = [](auto d) {
      struct x local{4};
      return x + local.a + static_cast<int>(d);
    };
    if (l.g() == 14 && lam() == 24 && glam(3) == 18) outer_ok = 1;
  }

  {
    struct fn {
      int a;
    };
    using M::fn;
    using M2::fn;
    auto glam = [](auto d) {
      struct fn local{2};
      return fn(d) + local.a;
    };
    if (glam(4) != 7 || glam(2.0) != 104) return 5;
  }

  return outer_ok ? 0 : 5;
}

constexpr int test_nested_blocks_constexpr() {
  struct x {
    int a;
    struct Inner {
      int v;
    };
  };
  {
    using M::x;
    struct x obj{6};
    x::Inner in{7};
    struct L {
      struct x member;
      x::Inner nested;
      constexpr int g() const { return x + member.a + nested.v; }
    };
    L l{{1}, {2}};
    if (obj.a != 6 || in.v != 7 || l.g() != 14) return 1;
  }
  return 0;
}
static_assert(test_nested_blocks_constexpr() == 0);

template <int K>
constexpr int test_in_template() {
  struct x {
    int a;
  };
  using M::x;
  struct L {
    struct x member;
    constexpr int g() const { return x + member.a + K; }
  };
  L l{{5}};
  return l.g();
}
static_assert(test_in_template<3>() == 19);

constexpr int test_typedef_different_name_and_elaborated() {
  struct x {
    int a;
  };
  typedef struct x y;
  using M::x;
  y obj{12};
  struct x* p = &obj;
  class x* q = &obj;
  struct [[maybe_unused]] AttrLocal;
  return (x == 11 && p->a == 12 && q->a == 12) ? 0 : 6;
}
static_assert(test_typedef_different_name_and_elaborated() == 0);

struct GlobalFunctor {
  constexpr int operator()(int v) const { return v + 999; }
};
template <typename T>
constexpr GlobalFunctor gfn{};

template <int N>
struct GlobalBox {
  constexpr GlobalBox(int) {}
  constexpr operator int() const { return 99; }
};

namespace M3 {
constexpr double x = 1.0;
constexpr int GlobalBox = 5;
template <typename T>
constexpr int gfn(T v) {
  return static_cast<int>(v) + 1;
}
enum En { e = 7 };
}  // namespace M3

constexpr int test_init_capture_hides_outer_using() {
  struct x {
    char c;
  };
  using M3::x;
  auto lam = [x = 'a']() {
    auto glam = [](auto d) { return sizeof(x) + sizeof(d) - sizeof(d); };
    struct L {
      constexpr int g() const { return static_cast<int>(sizeof(x)); }
    };
    return static_cast<int>(glam(1)) + L{}.g();
  };
  return lam();
}
static_assert(test_init_capture_hides_outer_using() == 2 * static_cast<int>(sizeof(char)));

constexpr int test_block_using_hides_global_var_template() {
  struct gfn {
    int a;
  };
  using M3::gfn;
  struct L {
    struct gfn m;
    constexpr int g() const { return gfn<int>(3) + m.a; }
  };
  return gfn<int>(3) + L{{2}}.g();
}
static_assert(test_block_using_hides_global_var_template() == 10);

constexpr int test_outer_block_hides_global_class_template() {
  struct GlobalBox {
    int a;
  };
  using M3::GlobalBox;
  struct L {
    struct GlobalBox m;
    constexpr int g() const { return ((GlobalBox < 10) > -1) + m.a; }
  };
  return L{{7}}.g();
}
static_assert(test_outer_block_hides_global_class_template() == 8);

constexpr int test_using_enum_beside_local_tag() {
  using M3::En;
  struct e {
    int a;
  };
  {
    using enum En;
    struct e obj{3};
    struct L {
      struct e m;
      constexpr int g() const { return e + m.a; }
    };
    return e + obj.a + L{{5}}.g();
  }
}
static_assert(test_using_enum_beside_local_tag() == 22);

template <class T>
constexpr int e(T v) {
  return static_cast<int>(v);
}

constexpr int test_using_enum_hides_global_template() {
  using M3::En;
  using enum En;
  struct L {
    constexpr int g() const { return (e < 10) ? 1 : 0; }
  };
  return (e < 10) + L{}.g();
}
static_assert(test_using_enum_hides_global_template() == 2);

namespace M4 {
template <class U>
struct CT {
  U a;
};
template <class T>
constexpr int gfn(T v) {
  return static_cast<int>(v) + 1;
}
}  // namespace M4

template <class CT>
constexpr int test_outer_block_using_over_template_param_type() {
  {
    using M4::CT;
    struct Local {
      CT<int> m;
    };
    return Local{{3}}.m.a;
  }
}

template <class gfn>
constexpr int test_outer_block_using_over_template_param_fn() {
  {
    using M4::gfn;
    struct Local {
      constexpr int g() const { return gfn<int>(3); }
    };
    return Local{}.g();
  }
}
static_assert(test_outer_block_using_over_template_param_type<double>() == 3);
static_assert(test_outer_block_using_over_template_param_fn<double>() == 4);

constexpr int test_nested_lambda_init_capture_hides_outer_tag() {
  struct x {
    char c[16];
  };
  using M3::x;
  auto lam1 = [x = 'a']() {
    auto lam2 = []() {
      auto glam = [](auto d) { return sizeof(x) + sizeof(d) - sizeof(d); };
      return glam(1);
    };
    return lam2();
  };
  return static_cast<int>(lam1());
}
static_assert(test_nested_lambda_init_capture_hides_outer_tag() == static_cast<int>(sizeof(char)));


