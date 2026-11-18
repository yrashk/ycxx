// [cpp.include]/3: when the search for a quoted name fails, the directive is reprocessed as if
// it read #include <...>, so a header only in an -I directory is still found.
// FLAGS: -Iinputs/angle
before;
#include "angle.h"
after;
