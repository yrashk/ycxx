#include "types.hpp"

extern "C" const CastDerived* identity(CastDerived* source) {
  return dynamic_cast<const CastDerived*>(source);
}

extern "C" const CastBase* upcast(CastDerived* source) {
  return dynamic_cast<const CastBase*>(source);
}
