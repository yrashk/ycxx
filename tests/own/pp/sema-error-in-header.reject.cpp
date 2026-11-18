// A semantic error in an included header is reported at its file and line in the header
// ([cpp.include]): its `//!` marker is in inputs/sema-error.h.
int before;
#include "inputs/sema-error.h"
int after;
