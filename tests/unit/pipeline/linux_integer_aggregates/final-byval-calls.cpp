#include "final-byval.hpp"

#define ZEROES_6 0, 0, 0, 0, 0, 0
#define ZEROES_7 ZEROES_6, 0
#define ZEROES_8 ZEROES_7, 0
#define ZEROES_30 ZEROES_8, ZEROES_8, ZEROES_8, ZEROES_6
#define ZEROES_31 ZEROES_8, ZEROES_8, ZEROES_8, ZEROES_7

int test_final_byval(FinalByvalCheck check, FinalByvalReverse reverse) {
  if (final_byval_check(ZEROES_31, Wide{41, 73})) return 1;
  Wide direct = final_byval_reverse(ZEROES_30, Wide{41, 73});
  if (direct.a != 73 || direct.b != 41) return 2;

  // Thirty-one scalars, or thirty scalars and a result destination, leave the record at bit 31.
  const bool wrong_argument = check(ZEROES_31, Wide{41, 73});
  Wide indirect = reverse(ZEROES_30, Wide{41, 73});
  const bool wrong_result = indirect.a != 73 || indirect.b != 41;
  return 4 * int(wrong_argument) + 8 * int(wrong_result);
}
