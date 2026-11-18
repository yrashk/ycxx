// [cpp.include], [cpp.scope]: macro definitions are not scoped to a file. A macro the includer
// defines is replaced in the header, and one the header defines is replaced in the includer.
#define INCLUDER_VALUE includer_value
#include "inputs/defines.h"
uses_header = HEADER_VALUE;
