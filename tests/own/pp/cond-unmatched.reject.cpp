// [cpp.cond]/1 (the if-section grammar): #else, #elif and #endif need an open #if.
int before;
#endif //! error
#else //! error
#elif 1 //! error
int after;
