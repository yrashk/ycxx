#include "base.hpp"
#include "types.hpp"

extern "C" const char __ycxx_linkage_static_v1;
extern "C" int ycxx_pal_debugger_present() noexcept;
extern "C" const D* polymorphic_identity(D* source);

int main() {
  D owned;
  owned.field = 41;
  if (dispatch(owned) != 41) return 1;
  owned.field = -29;
  if (dispatch(owned) != -29) return 2;
  CastDerived ordinary{{13}, 29};
  const auto* base = upcast(&ordinary);
  if (identity(&ordinary) != &ordinary || base != static_cast<CastBase*>(&ordinary) || base->base != 13) return 3;
  if (identity(nullptr) != nullptr || upcast(nullptr) != nullptr) return 4;
  if (__ycxx_linkage_static_v1 != 0 || ycxx_pal_debugger_present() != 0) return 5;
  if (polymorphic_identity(&owned) != &owned || polymorphic_identity(nullptr) != nullptr) return 6;
  return 0;
}
