// #211: `__underlying_type` of a type that is no enumeration, and `__is_empty` of an incomplete
// class (docs/EXTENSIONS.md, [meta.trans.other], [meta.unary.prop]).
struct Incomplete;
using U = __underlying_type(int); //! error
constexpr bool b = __is_empty(Incomplete); //! error
int main() { return 0; }
