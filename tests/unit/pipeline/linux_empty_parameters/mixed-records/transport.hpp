#pragma once

using Word = unsigned long;
struct Tag {
  explicit Tag(int& count) noexcept;
};
struct Pair {
  Word low;
  Word high;
};
struct Byte {
  unsigned char value;
};

Word transport(Word a, Word b, Word c, Word d, Word e, Tag tag,
               Pair pair, int&& ref, Byte byte, Word last) noexcept;
