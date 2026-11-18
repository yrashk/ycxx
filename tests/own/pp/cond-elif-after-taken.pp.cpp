// [cpp.cond]/13: once a group of the chain is processed, the following #elif, #elifdef and
// #elifndef directives are processed as if in a skipped group, so their conditions are not
// evaluated: a division by zero or a malformed expression there is not an error.
#if 1
first_taken;
#elif 1 / 0
not_kept;
#elif )
not_kept;
#elifdef
not_kept;
#else
not_kept;
#endif
#if 0
#elif 2
second_taken;
#elif 1 % 0
not_kept;
#endif
end;
