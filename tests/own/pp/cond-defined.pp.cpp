// [cpp.cond]/2, /9: `defined identifier` and `defined ( identifier )` are 1 when the name is
// currently defined as a macro (even with an empty replacement), else 0; they combine with the
// other operators; a defined function-like macro counts too.
#define EMPTY
#define FN(x) x
#define ZERO 0
#if defined EMPTY
defined_empty;
#endif
#if defined(FN) && defined ( ZERO )
defined_parens;
#endif
#if !defined(UNDEFINED_NAME)
not_defined;
#endif
#if defined UNDEFINED_NAME || defined ZERO
or_defined;
#endif
#if defined(ZERO) + defined(EMPTY) + defined(FN) == 3
sum_three;
#endif
#if defined ZERO && ZERO
not_kept;
#else
zero_is_defined_but_false;
#endif
#undef EMPTY
#if defined EMPTY
not_kept;
#else
undefined_again;
#endif
end;
