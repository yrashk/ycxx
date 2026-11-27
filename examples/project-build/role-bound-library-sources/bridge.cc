#include "score.hh"

#if __has_include("detail.hh")
#error detail.hh is private to the generated implementation
#endif

int implementation_score();

int bridge_score() {
  return implementation_score();
}
