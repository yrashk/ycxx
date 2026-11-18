// [cpp.cond]/9: the #if condition is macro-replaced (except the operands of defined) before
// evaluation; object-like and function-like macros expand, a macro may expand to an
// expression with operators (with no implicit parentheses), and expansions nest.
#define VERSION 3
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SUM VERSION + 2
#define IS_NEW(v) ((v) >= 3)
#if VERSION == 3
object_like;
#endif
#if MIN(VERSION, 7) == 3
function_like;
#endif
#if SUM * 2 == 7
no_implicit_parens;
#endif
#if IS_NEW(VERSION) && !IS_NEW(2)
predicate;
#endif
#define VERSION_IS(v) (VERSION == v)
#if VERSION_IS(3)
nested_expansion;
#elif VERSION_IS(4)
not_kept;
#endif
end;
