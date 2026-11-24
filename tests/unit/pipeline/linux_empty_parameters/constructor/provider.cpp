#include "probe.hpp"

Tag::Tag(int& evaluated) noexcept { ++evaluated; }

Choice::Choice(Tag tag, const Tag* original, int&& input,
               int a, int b, int c, int d, int e, int f) noexcept
    : value(&tag == original ? -1 : input + a + 2*b + 3*c + 4*d + 5*e + 6*f) {}
