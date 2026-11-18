// [cpp.replace.general]/10, [cpp.rescan]/1: an object-like macro name is replaced by its
// replacement list, which is rescanned for more macro names; a macro defined later than
// another that names it is still used, since replacement happens at the point of use.
#define ONE 1
#define TWO ONE + ONE
#define FOUR TWO + TWO
#define LATER EARLIER_USE
#define EARLIER_USE defined_after
int a = ONE;
int b = TWO;
int c = FOUR;
int d = LATER;
int e = not_a_macro;
ONE ONE TWO
