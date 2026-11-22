// INTEGER records by value through member functions, static members, function pointers,
// templates and closures (#640): the placement of ordinary functions, with `this` first.
#include "shapes.hpp"

struct Calc {
  long base;
  Pair swap(Pair p) const;
  Wide add(Wide w, long k);
  Three spill(long, long, long, long, long, Three t) const;
  static Small make(unsigned char a);
  long rollback(long, long, long, long, Wide w, long last) const;
};

using Swapper = Pair (*)(Pair);
using Shifter = int (*)(Three, Wide);
Swapper swapper();
using Widen = Wide (*)(Wide);
Widen widener();
int apply(Shifter f, Three t, Wide w);
