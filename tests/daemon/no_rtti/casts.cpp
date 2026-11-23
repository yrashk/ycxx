#include "types.hpp"

extern "C" const D* identity(D* source) {
  return dynamic_cast<const D*>(source);
}

extern "C" const B* upcast(D* source) {
  return dynamic_cast<const B*>(source);
}
