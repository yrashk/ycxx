// [cpp.error]/1: #error in a taken group makes the program ill-formed, with or without
// message tokens; in a skipped group it is not processed.
#if 0
#error skipped
#endif
#error a message with 'quotes' and "strings" //! error
int between;
#error //! error
