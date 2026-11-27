#include "score.hh"

extern "C" int archive_score();

int bridge_score() {
  return archive_score();
}
