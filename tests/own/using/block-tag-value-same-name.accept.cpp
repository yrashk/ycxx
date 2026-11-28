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
  return (x == 11 && p->a == 12 && q->a == 12) ? 0 : 6;
}
static_assert(test_typedef_different_name_and_elaborated() == 0);
