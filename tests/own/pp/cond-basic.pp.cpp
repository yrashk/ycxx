// [cpp.cond]/8, /12, /13: of a #if/#elif/#else chain exactly the first group whose condition is
// nonzero is kept (the #else group when none is), the others are skipped; #ifdef and #ifndef
// test whether a name is currently defined as a macro.
#define ONE 1
#if 1
if_taken;
#else
else_skipped;
#endif
#if 0
if_skipped;
#elif 0
elif_skipped;
#elif 2
second_elif_taken;
#elif 1
later_elif_skipped;
#else
else_skipped;
#endif
#if 0
skipped;
#else
else_taken;
#endif
#if 0
nothing_kept;
#endif
#ifdef ONE
ifdef_taken;
#else
ifdef_else_skipped;
#endif
#ifndef ONE
ifndef_skipped;
#else
ifndef_else_taken;
#endif
#ifdef NOT_A_MACRO
ifdef_skipped;
#endif
#ifndef NOT_A_MACRO
ifndef_taken;
#endif
#undef ONE
#ifdef ONE
after_undef_skipped;
#else
after_undef_taken;
#endif
end;
