// An array element cannot itself be an array of unknown bound ([dcl.array], #222).
using InvalidInner = int[2][]; //! error
void invalid_group(int (*p)[2][]); //! error
