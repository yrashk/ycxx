// __COUNTER__ (not in the pinned draft's [cpp.predefined]; proposed for C++ and provided by both
// oracles) expands to 0 on its first use in a translation unit and to one more on each later
// use, including uses inside macros and #if.
#define NEXT __COUNTER__
a __COUNTER__;
b __COUNTER__ __COUNTER__;
c NEXT;
#if __COUNTER__ == 4
counted_in_if;
#endif
d __COUNTER__;
