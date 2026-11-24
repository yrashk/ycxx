// #257: unqualified lookup finds the inherited member before the namespace
// declaration ([basic.lookup.unqual], [class.member.lookup]).
constexpr bool enabled = true;

struct Base {
  static constexpr bool enabled = false;
};

struct Derived : Base {
  static_assert(enabled, "inherited flag is false"); //! error
};
