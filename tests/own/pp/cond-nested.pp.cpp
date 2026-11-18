// [cpp.cond]/13 (and the if-section grammar): conditionals nest; inside a skipped group, nested
// conditionals are only tracked for their nesting (their conditions are not evaluated, so an
// invalid expression there is not an error), and a nested group is kept only when every
// enclosing group is.
#define X 1
#if X
outer_taken;
# if 0
inner_skipped;
# elif X
inner_elif_taken;
# endif
# ifdef X
#  ifndef Y
deep_taken;
#  endif
# endif
#else
# if 1
inner_of_skipped_outer;
# else
also_skipped;
# endif
#endif
#if 0
# if 1 / 0 + )
never_evaluated;
# elif (
never_evaluated_either;
# else
skipped_else;
# endif
#elif 1
outer_elif_taken;
#endif
end;
