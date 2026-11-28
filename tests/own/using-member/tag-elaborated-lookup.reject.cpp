// Qualified elaborated class lookup and ordinary tag-hiding rejection controls
// ([basic.lookup.elab], [dcl.type.elab]/2, [class.member.lookup], [namespace.udecl], #1554).
struct B {
  int f() const { return 1; }
};
struct Tag {};

struct HasAlias {
  using f = Tag;
};
struct HasAlias::f* bad_elab_alias;  //! error

struct HasEnum {
  enum f { a };
};
struct HasEnum::f* bad_elab_enum;  //! error

struct HasStruct {
  struct f {};
};
union HasStruct::f* bad_elab_union;  //! error

struct HasPrivate {
private:
  struct f {};
};
struct HasPrivate::f* bad_elab_priv;  //! error

struct BaseWithTag {
  struct f {};
};
struct PrivBaseDer : private BaseWithTag {};
struct PrivBaseDer::f* bad_elab_priv_base;  //! error

// Ordinary qualified lookup `D::f` is hidden by introduced `B::f` (both incomplete and complete).
struct DTagAndFn : B {
  struct f {};
  using B::f;
  using BadIncomplete = DTagAndFn::f;  //! error
};
DTagAndFn::f bad_ordinary_hidden;  //! error

// Unqualified hidden tag in a class template specialization must not be usable as a type.
struct BStatic {
  static constexpr int sf() { return 2; }
};
template<class T> struct THiddenCast : BStatic {
  struct sf { int v = 10; };
  using BStatic::sf;
  int bad() const { return sf{}.v; }  //! error
};
int trigger_hidden_cast(THiddenCast<int> x) { return x.bad(); }

// Multiple inheritance mixing a base tag with a typedef-name in another branch is ill-formed.
struct MIBase { struct f {}; };
struct MILeft : MIBase { using f = MIBase::f; };
struct MIRight : MIBase {};
struct MIDerived : MILeft, MIRight {};
struct MIDerived::f* bad_mi_alias_tag = nullptr;  //! error
