// [cpp.include]/3, /6: a header in a subdirectory includes "sibling.h" and "../inner.h";
// both are found relative to that header's own directory, inputs/sub/.
before;
#include "inputs/sub/rel.h"
after;
