#include "volatile-class.hpp"

int peer_take(Implicit i) { i.k += 1; return i.k; }
int peer_take(Defaulted d) { d.k += 2; return d.k; }
int peer_take(Derived d) { d.d += 3; return d.k + d.d; }
int peer_take(Member m) { m.d += 4; return m.i.k + m.d; }

int peer_calls() { return take(Implicit{}) + 10 * take(Defaulted{}) + 100 * take(Derived{}) + 1000 * take(Member{}); }
