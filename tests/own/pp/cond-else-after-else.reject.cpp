// [cpp.cond]/1 (the if-section grammar): an if-section has at most one #else, and it comes
// after every #elif.
#if 0
#else
#else //! error
#endif
#if 1
#else
#elif 1 //! error
#endif
int after;
