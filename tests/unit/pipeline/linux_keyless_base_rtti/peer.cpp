#include "base.hpp"

#include <cstddef>
#include <cstdint>
#include <typeinfo>

extern "C" D* keyless_downcast(B*);
extern "C" const volatile D* keyless_qualified_downcast(const volatile B*);
extern "C" void* __real___dynamic_cast(const void*, const void*, const void*, std::ptrdiff_t);

int D::f() { return payload + 20; }

namespace {
  struct Primary {
    virtual int identify() { return 1; }
    int marker = 19;
  };

  struct Most : Primary, D {};

  struct Sibling : B {
    int f() override { return 83; }
  };

  struct Observation {
    const volatile B* source;
    int calls = 0;
    int evaluations = 0;
    bool arguments_ok = true;
  };

  Observation observed{};

  bool downcasts(B* source, D* expected) {
    const int calls = source == nullptr ? 0 : 1;
    observed = {source};
    if (keyless_downcast(source) != expected || !observed.arguments_ok ||
        observed.calls != calls || observed.evaluations != 0)
      return false;

    observed = {source};
    return keyless_qualified_downcast(source) == expected && observed.arguments_ok &&
           observed.calls == calls && observed.evaluations == 1;
  }
}

extern "C" const volatile B* observe_keyless_operand(const volatile B* source) {
  ++observed.evaluations;
  return source;
}

// Itanium ABI §2.9.7. Check the original subobject pointer and the linked type_info addresses.
extern "C" void* __wrap___dynamic_cast(const void* original, const void* source_type,
                                     const void* target_type, std::ptrdiff_t hint) {
  ++observed.calls;
  observed.arguments_ok = observed.arguments_ok && original == observed.source &&
                          source_type == &typeid(B) && target_type == &typeid(D) && hint == 0;
  if (!observed.arguments_ok) return nullptr;
  return __real___dynamic_cast(original, source_type, target_type, hint);
}

int main() {
  D object;
  object.payload = 10;
  if (object.f() != 30 || !downcasts(&object, &object)) return 1;

  // [expr.dynamic.cast] finds the unique D even when it is interior to a peer-owned object.
  Most most;
  D* displaced = &most;
  B* base = displaced;
  displaced->payload = 22;
  if (static_cast<void*>(base) != static_cast<void*>(displaced) ||
      reinterpret_cast<std::uintptr_t>(displaced) <= reinterpret_cast<std::uintptr_t>(&most))
    return 2;
  if (displaced->f() != 42 || !downcasts(base, displaced)) return 3;

  Sibling sibling;
  if (!downcasts(&sibling, nullptr)) return 4;

  B* null_source = nullptr;
  if (!downcasts(null_source, nullptr)) return 5;
}
