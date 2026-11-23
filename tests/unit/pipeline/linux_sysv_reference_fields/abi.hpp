#pragma once

struct OneRef {
  int& value;
};

struct TwoRefs {
  int& first;
  int& second;
};

static_assert(sizeof(OneRef) == 8 && alignof(OneRef) == 8);
static_assert(sizeof(TwoRefs) == 16 && alignof(TwoRefs) == 8);

int direct_one(OneRef argument, const OneRef* original, int tail);
int direct_two(TwoRefs argument, const TwoRefs* original, int tail);

using Rollback = int (*)(int, int, int, int, int, TwoRefs, int);
Rollback rollback_target();

extern "C" int ref_captures(bool heapless);
extern "C" int closure_parameter_objects();
extern "C" int value_captures();
