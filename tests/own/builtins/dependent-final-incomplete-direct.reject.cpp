// #1042 [meta.unary.prop] requires a complete class operand.
struct Later;
constexpr bool direct = __is_final(Later); //! error
struct Later final {};
static_assert(__is_final(Later));
