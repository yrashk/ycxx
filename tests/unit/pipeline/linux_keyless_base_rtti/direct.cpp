#include "base.hpp"

extern "C" D* keyless_downcast(B* source) {
  return dynamic_cast<D*>(source);
}
