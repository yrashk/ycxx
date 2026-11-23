#include "types.hpp"

int Owner::step() { return payload + 1; }

extern "C" int dispatch(Root* source) { return source->step(); }

extern "C" const void* complete_address() {
  return dynamic_cast<const void*>(borrow());
}

extern "C" const Owner* identity(Owner* source) {
  return dynamic_cast<const Owner*>(source);
}

extern "C" const Root* upcast(Owner* source) {
  return dynamic_cast<const Root*>(source);
}
