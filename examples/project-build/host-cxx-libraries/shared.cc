#include "left_score.hh"
#include "right_score.hh"
#include "shared_score.hh"

int shared_score() {
  return left_score() + right_score();
}
