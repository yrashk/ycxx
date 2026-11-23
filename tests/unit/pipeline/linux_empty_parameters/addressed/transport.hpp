#pragma once

using Word = unsigned long long;
void record(int id) noexcept;

struct Tag {
  Tag(int id) noexcept { record(id); }
};

bool same_tag(const Tag *, const Tag *) noexcept;
Word transport(Word a, Tag first, Word b, Word c, Tag second, Word d,
               Word e, Word f, Word g, Word h) noexcept;
