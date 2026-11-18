// [cpp.cond]/1 (the if-section grammar), /11: the #if operand must form a constant-expression:
// a missing operand, unbalanced parentheses and an empty condition are ill-formed.
#if 1 + //! error
#endif
#if (1 //! error
#endif
#if 1) //! error
#endif
#if //! error
#endif
#if 1 2 //! error
#endif
int after;
