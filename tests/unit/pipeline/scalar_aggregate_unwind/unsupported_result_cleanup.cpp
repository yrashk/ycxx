#include "result.hpp"

void note(int) noexcept;

struct Guard {
  int stamp;
  ~Guard() noexcept { note(stamp); }
};

Small cleanup_result(int input) {
  Guard guard{41};
  return {result_field(input)};
}
