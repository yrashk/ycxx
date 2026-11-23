// #1046 [meta.unary.prop] requires a complete class operand, including a union.
union Pending;
constexpr bool is_sealed = __is_final(Pending); //! error
