// #257: an unevaluated operand may name an earlier non-static data member
// ([expr.prim.id.general], [expr.sizeof]).
struct MemberSize {
  int value;
  static_assert(sizeof(value) == sizeof(int));
};

// Unqualified lookup searches the base before the enclosing namespace
// ([basic.lookup.unqual], [class.member.lookup]).
constexpr bool enabled = false;

struct Base {
  static constexpr bool enabled = true;
};

struct Derived : Base {
  static_assert(enabled);
};
