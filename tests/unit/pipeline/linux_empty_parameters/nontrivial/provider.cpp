#include "transport.hpp"

Tag::Tag(int value) noexcept { events = value; }
Tag::~Tag() noexcept { events = events * 10 + 7; }

Word transport(Word a, Word b, Word c, Word d, Word e, Tag,
               int&& ref, Word sentinel) noexcept {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + ref + sentinel + static_cast<Word>(events);
}
