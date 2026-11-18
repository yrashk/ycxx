// [cpp.cond]/11, [expr.mul], [expr.const.core]: a division or remainder by zero that is
// evaluated makes the #if expression not a constant expression.
#if 1 / 0 //! error
#endif
#if 5 % 0 //! error
#endif
#if 1 && 2 / (1 - 1) //! error
#endif
int after;
