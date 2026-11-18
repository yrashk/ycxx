// [lex.pptoken]/2, [lex.phases], [cpp.cond]/13: a skipped group is still decomposed into
// preprocessing tokens (phase 3 precedes phase 4), and an unmatched ' or " that would form a
// single-character preprocessing token makes the program ill-formed, skipped or not.
// XFAIL: clang Clang does not diagnose an unmatched quote inside a skipped group
#if 0
it's an unmatched quote //! error
#endif
int after;
