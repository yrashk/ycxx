#include "base.hpp"

extern "C" const D* polymorphic_identity(D* source) {
  return dynamic_cast<const D*>(source);
}
