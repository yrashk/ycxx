#include "transport.hpp"

Word transport(Word a, Tag first, Word b, Word c, Tag second, Word d,
               Word e, Word f, Word g, Word h) noexcept {
  if (same_tag(&first, &second)) return 0;
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}
