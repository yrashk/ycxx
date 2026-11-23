#include "abi.hpp"

int direct_one(OneRef argument, const OneRef* original, int tail) {
  if (&argument == original || &argument.value != &original->value) return -1;
  argument.value = argument.value + tail;
  return argument.value;
}

int direct_two(TwoRefs argument, const TwoRefs* original, int tail) {
  if (&argument == original) return -2;
  if (&argument.first != &original->first || &argument.second != &original->second) return -3;
  argument.first = argument.first + tail;
  argument.second = argument.second - tail;
  return argument.second - argument.first;
}

static int after_five(int a, int b, int c, int d, int e, TwoRefs argument, int tail) {
  if (a != 2 || b != 3 || c != 5 || d != 7 || e != 11 || tail != 23) return -4;
  argument.first = argument.first + tail;
  argument.second = argument.second - e;
  return argument.first + 2 * argument.second;
}

Rollback rollback_target() {
  return &after_five;
}
