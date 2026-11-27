#include "schema.hh"

#if __has_include("wire_score.hh")
#error wire_score.hh is private to the generated source
#endif

int wire_score();

int schema_score() {
  return wire_score();
}
