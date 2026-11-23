#pragma once

struct Tag {
  explicit Tag(int value) noexcept;
  ~Tag() noexcept;
};

extern int events;
using Word = unsigned long;
Word transport(Word a, Word b, Word c, Word d, Word e, Tag tag,
               int&& ref, Word sentinel) noexcept;
