#include "cross-object.hpp"

int peer_layout(int i) { return layout(i); }

Triple peer_swap(Triple t) {
  Triple r = t;
  r.a = t.b;
  r.b = t.a;
  return r;
}

Indirect peer_next(Indirect i) {
  Indirect r(i);
  r.pad += 1;
  return r;
}

int peer_sum(Indirect i) { return i.c.v + i.pad; }

long peer_first(MayThrow m, long b) { return m.a * 10 + b; }

Volatile peer_volatile(Volatile v) { return Volatile(v.x + 1); }

// A prvalue argument: the caller constructs the object it passes.
int peer_derived() { return take_derived(DerivedVolatile()) ; }
