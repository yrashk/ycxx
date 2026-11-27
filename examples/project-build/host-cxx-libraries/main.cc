#include "generated_config.hh"
#include "shared_score.hh"

static_assert(generated_score == expected_score);

int main() {
  return shared_score() == generated_score ? generated_score : 1;
}
