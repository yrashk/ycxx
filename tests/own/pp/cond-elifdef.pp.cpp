// [cpp.cond]/12, /13: #elifdef and #elifndef are #elif defined and #elif !defined; like #elif they
// are only considered when no earlier group of the chain was kept.
#define A
#if 0
skipped;
#elifdef B
skipped_b;
#elifdef A
elifdef_a_taken;
#else
else_skipped;
#endif
#if 0
skipped;
#elifndef A
skipped_a;
#elifndef B
elifndef_b_taken;
#endif
#if 1
if_taken;
#elifdef A
elifdef_after_taken_group_skipped;
#endif
#ifdef B
skipped;
#elifndef B
ifdef_chain_elifndef_taken;
#endif
end;
