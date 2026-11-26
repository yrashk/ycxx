#include "shared_score.hh"

int main() {
  const int score = shared_score();
  return score == expected_score ? score : 1;
}
