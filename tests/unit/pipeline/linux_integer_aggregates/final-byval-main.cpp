#include "final-byval.hpp"

int final_byval_check(LONGS_31, Wide w) { return w.a != 41 || w.b != 73; }
Wide final_byval_reverse(LONGS_30, Wide w) { return Wide{w.b, w.a}; }

int main() { return test_final_byval(final_byval_check, final_byval_reverse); }
