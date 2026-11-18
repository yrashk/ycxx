// [cpp.cond]/11, [expr.log.and], [expr.log.or], [expr.cond]: the unevaluated operand of &&,
// || and ?: is not evaluated, so a division by zero there is not an error.
#if 0 && 1 / 0
not_kept;
#else
and_short_circuit;
#endif
#if 1 || 1 / 0
or_short_circuit;
#endif
#if 1 ? 2 : 1 % 0
cond_false_branch_unevaluated;
#endif
#if (0 ? 1 / 0 : 3) == 3
cond_true_branch_unevaluated;
#endif
#if defined(NOPE) && 1 / NOPE
not_kept;
#else
guarded_division;
#endif
end;
