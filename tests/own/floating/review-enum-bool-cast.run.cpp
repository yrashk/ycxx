// [expr.static.cast]/10, [conv.fpint], from #336's review: a floating value cast to an
// enumeration converts to its underlying type by truncation, even when that type is bool, both
// when it is a constant and when it is computed at run time.
// STDOUT: 0 0 1 1
#include <stdio.h>
enum B : bool { bf, bt };
double half() { return 0.5; }
double one_half() { return 1.5; }
int main() {
  printf("%d %d %d %d\n", (int)static_cast<B>(0.5), (int)static_cast<B>(half()), (int)static_cast<B>(1.5),
         (int)static_cast<B>(one_half()));
  return 0;
}
