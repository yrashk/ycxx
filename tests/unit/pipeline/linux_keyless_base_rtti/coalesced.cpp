#include "base.hpp"

struct E : B {
  int payload;
  int f() override;
};

int E::f() { return payload; }

extern "C" D* coalesced_downcast(B* source) {
  return dynamic_cast<D*>(source);
}
