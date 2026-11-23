#include "types.hpp"

extern "C" const char __ycxx_linkage_static_v1;
extern "C" int ycxx_pal_debugger_present() noexcept;

struct Lead {
  virtual int anchor() { return 0; }
  int pad;
};

struct Most : Lead, Root {
  int step() override { return 73; }
};

static const Root* operand;
static int evaluations;

extern "C" const Root* borrow() {
  ++evaluations;
  return operand;
}

int main() {
  Owner owned;
  owned.payload = 41;
  if (dispatch(&owned) != 42) return 1;

  Most most;
  Root* secondary = &most;
  if (static_cast<const void*>(secondary) == static_cast<const void*>(&most)) return 2;
  operand = secondary;
  if (complete_address() != &most || evaluations != 1 || dispatch(secondary) != 73) return 3;

  operand = nullptr;
  if (complete_address() != nullptr || evaluations != 2) return 4;
  if (__ycxx_linkage_static_v1 != 0 || ycxx_pal_debugger_present() != 0) return 5;
  if (identity(&owned) != &owned || upcast(&owned) != static_cast<Root*>(&owned)) return 6;
  if (identity(nullptr) != nullptr || upcast(nullptr) != nullptr) return 7;
  return 0;
}
