// A system header (found through -isystem): its warnings are not reported, nor are those of the
// headers it includes, from its own directory or found through -I. A macro it defines reports
// where it is used.
#pragma once
#include "noisy-sibling.h"
#include <pulled-in.h>
#ifdef NOISY_UNDEFINED
#endif NOISY_UNDEFINED
#define NOISY_MULTICHAR 'kl'
inline int noisy() { return 'ab' == 0 ? 0 : sibling() + pulled_in(); }
