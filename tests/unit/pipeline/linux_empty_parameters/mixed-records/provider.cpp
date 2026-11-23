#include "transport.hpp"

Tag::Tag(int& count) noexcept { ++count; }

Word transport(Word a, Word b, Word c, Word d, Word e, Tag,
               Pair pair, int&& ref, Byte byte, Word last) noexcept {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * pair.low + 7 * pair.high
         + 8 * static_cast<Word>(ref) + 9 * byte.value + 10 * last;
}
