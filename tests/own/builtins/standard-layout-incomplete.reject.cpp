// #1030 [meta.unary.prop], [dcl.enum].
struct Later;
constexpr bool record = __is_standard_layout(Later); //! error
constexpr bool array = __is_standard_layout(Later[2]); //! error
struct Later { int value; };
static_assert(__is_standard_layout(Later));

enum Unfixed {
  from_trait = __is_standard_layout(Unfixed) //! error
};
