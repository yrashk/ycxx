// Nested class/union/enum tags may share a name with an introduced base member function,
// and qualified elaborated lookup (`struct D::f`, `union D::f`, `enum D::f`) resolves the tag
// ([namespace.udecl], [basic.scope.scope]/4, [basic.lookup.elab], [class.member.lookup], #1554).
struct B {
  constexpr int f() const { return 1; }
  static constexpr int sf() { return 2; }
};

// Tag declared before `using B::f;`.
struct D : B {
  struct f { int v = 3; };
  using B::f;
  constexpr int inside() const {
    struct f local{};
    struct D::f qual{};
    return f() + local.v + qual.v;
  }
};
constexpr struct D::f d_tag{};
static_assert(d_tag.v == 3);

// Tag declared after `using B::f;`.
struct DRev : B {
  using B::f;
  struct f { int v = 4; };
};
constexpr struct DRev::f drev_tag{};
static_assert(drev_tag.v == 4);

// `class`, `union`, `enum`, and `enum class` tags sharing a name with `using B::f;`.
struct DClass : B {
  class f { public: int v = 5; };
  using B::f;
};
constexpr class DClass::f dclass_tag{};
static_assert(dclass_tag.v == 5);

struct DUnion : B {
  union f { int v; char c; };
  using B::f;
};
constexpr union DUnion::f dunion_tag{6};
static_assert(dunion_tag.v == 6);

struct DEnum : B {
  enum f { a = 7 };
  using B::f;
};
constexpr enum DEnum::f denum_tag = DEnum::a;
static_assert(denum_tag == 7);

struct DScopedEnum : B {
  enum class f { a = 8 };
  using B::f;
};
constexpr enum DScopedEnum::f dscoped_tag = DScopedEnum::f::a;
static_assert(static_cast<int>(dscoped_tag) == 8);

// Multi-declarator using-declaration with a coexisting tag.
struct DMulti : B {
  struct f { int v = 9; };
  using B::f, B::sf;
};
constexpr struct DMulti::f dmulti_tag{};
static_assert(DMulti::sf() == 2 && dmulti_tag.v == 9);

// Elaborated qualified lookup in an incomplete class and past non-type members.
struct SelfElab {
  struct f { int v = 10; };
  struct SelfElab::f* p = nullptr;
};
struct NonTypeHide {
  struct f { int v = 11; };
  int f = 0;
};
constexpr struct NonTypeHide::f nth_tag{};
static_assert(nth_tag.v == 11);

struct BaseNested {
  struct f { int v = 12; };
};
struct DerHide : BaseNested {
  int f = 0;
};
constexpr struct DerHide::f dh_tag{};
static_assert(dh_tag.v == 12);

// Class templates with dependent and concrete bases.
template<class T>
struct TDep : T {
  struct f { int v = 13; };
  using T::f;
  struct TDep::f* self_ptr = nullptr;
};
constexpr struct TDep<B>::f tdep_tag{};
static_assert(tdep_tag.v == 13);

template<class T>
struct TConcrete : B {
  struct f { int v = 14; };
  using B::f;
};
constexpr struct TConcrete<int>::f tconc_tag{};
static_assert(tconc_tag.v == 14);

// Static base member function coexisting with a nested tag is directly constexpr-callable.
struct DStatic : B {
  struct sf { int v = 15; };
  using B::sf;
  static constexpr int qual_call = DStatic::sf();
};
constexpr struct DStatic::sf dstatic_tag{};
static_assert(DStatic::sf() == 2 && DStatic::qual_call == 2 && dstatic_tag.v == 15);

// Incomplete-class `sizeof(D::f)` sees the static data member, not the hidden nested tag.
struct IncompleteStaticHide {
  struct f { long long a, b; };
  static constexpr int f = 0;
  static constexpr int sz = static_cast<int>(sizeof(IncompleteStaticHide::f));
};
static_assert(IncompleteStaticHide::sz == static_cast<int>(sizeof(int)));

// Template member functions and static initializers call introduced `f()` / `sf()`, not hidden tag.
template<class T>
struct TDepCall : T {
  struct f { int v = 10; };
  using T::f;
  constexpr int m() const { return f(); }
};
template<class T>
struct TConcreteStatic : B {
  struct sf { int v = 10; };
  using B::sf;
  static constexpr int val = sf();
};
static_assert(TConcreteStatic<int>::val == 2);

// Dependent current instantiation elaborated lookup searches nondependent bases.
template<class U>
struct TBaseElab : BaseNested {
  struct TBaseElab::f* p1 = nullptr;
  struct TBaseElab::BaseNested* p2 = nullptr;
};
TBaseElab<int> tbase_elab_inst;

int check(D d, DRev dr, DClass dc, DUnion du, DEnum de, DScopedEnum dse, DMulti dm,
          TDep<B> td, TConcrete<int> tc, TDepCall<B> tdc) {
  return d.f() + d.inside() + dr.f() + dc.f() + du.f() + de.f() + dse.f() + dm.f() +
         td.f() + tc.f() + tdc.m();
}
