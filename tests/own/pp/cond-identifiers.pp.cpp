// [cpp.cond]/10, /11: after macro replacement, every remaining identifier except `true` and
// `false` (even one spelled like a keyword) is replaced with 0, so a name that is not a macro is
// false, and so is a macro expanding to an unknown name; `true` is 1 and `false` 0 after
// integral promotion. Alternative tokens (`and`, `not`) are operators, not identifiers.
#define ALIAS unknown_name
#if true
true_is_one;
#endif
#if false
not_kept;
#else
false_is_zero;
#endif
#if true + true == 2
true_plus_true;
#endif
#if not_a_macro
not_kept;
#else
unknown_is_zero;
#endif
#if not_a_macro == 0 && another_one + 3 == 3
unknown_in_arithmetic;
#endif
#if ALIAS
not_kept;
#else
alias_to_unknown_is_zero;
#endif
#if int || nullptr || this
not_kept;
#else
keywords_are_zero;
#endif
#if 1 and not 0 and (2 bitand 3) == 2
alternative_tokens;
#endif
end;
