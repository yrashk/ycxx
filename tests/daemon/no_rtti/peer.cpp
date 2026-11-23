#include "types.hpp"

extern "C" const char __ycxx_linkage_static_v1;
extern "C" int ycxx_pal_debugger_present() noexcept;

int main() {
  D owned;
  owned.field = 41;
  if (dispatch(owned) != 41) return 1;
  owned.field = -29;
  if (dispatch(owned) != -29) return 2;
  if (identity(&owned) != &owned || upcast(&owned) != static_cast<B*>(&owned)) return 3;
  if (identity(nullptr) != nullptr || upcast(nullptr) != nullptr) return 4;
  if (__ycxx_linkage_static_v1 != 0 || ycxx_pal_debugger_present() != 0) return 5;
  return 0;
}
