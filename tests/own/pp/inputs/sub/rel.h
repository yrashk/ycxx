// In a subdirectory: its quoted includes are found relative to inputs/sub/, first.
rel_before;
#include "sibling.h"
#include "../inner.h"
rel_after;
