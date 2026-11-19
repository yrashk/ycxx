// A system header (found through -isystem): its warnings are not reported, nor are those of the
// header it includes from its own directory.
#pragma once
#include "noisy-sibling.h"
inline int noisy() { return 'ab' == 0 ? 0 : sibling(); }
