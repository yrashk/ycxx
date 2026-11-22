#include "shapes.hpp"

#define LONGS_6 long, long, long, long, long, long
#define LONGS_7 LONGS_6, long
#define LONGS_8 LONGS_7, long
#define LONGS_30 LONGS_8, LONGS_8, LONGS_8, LONGS_6
#define LONGS_31 LONGS_8, LONGS_8, LONGS_8, LONGS_7

using FinalByvalCheck = int (*)(LONGS_31, Wide);
using FinalByvalReverse = Wide (*)(LONGS_30, Wide);

int final_byval_check(LONGS_31, Wide);
Wide final_byval_reverse(LONGS_30, Wide);
int test_final_byval(FinalByvalCheck, FinalByvalReverse);
