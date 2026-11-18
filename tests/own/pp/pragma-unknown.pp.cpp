// [cpp.pragma]/1: a #pragma the implementation does not recognize is ignored by translation;
// in -E output both oracles pass it through as a directive line, unexpanded.
#define X expanded
before;
#pragma ycxx_unknown_pragma some tokens X
#pragma totally_unknown(1, 2)
after X;
