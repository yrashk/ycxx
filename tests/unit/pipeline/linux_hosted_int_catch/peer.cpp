extern "C" int typed_handler(int);
extern "C" void* __real___cxa_begin_catch(void*) noexcept;
extern "C" void __real___cxa_end_catch();

namespace {
  int cleanups;
  int begins;
  int ends;

  struct Guard {
    ~Guard() { ++cleanups; }
  };

  bool observed(int expected_begins, int expected_ends) {
    return cleanups == 1 && begins == expected_begins && ends == expected_ends;
  }

  void reset() { cleanups = begins = ends = 0; }
}

// Itanium EH ABI §2.5.3. Count the calls without changing the runtime's pointers or state.
extern "C" void* __wrap___cxa_begin_catch(void* header) noexcept {
  ++begins;
  return __real___cxa_begin_catch(header);
}

extern "C" void __wrap___cxa_end_catch() {
  ++ends;
  __real___cxa_end_catch();
}

extern "C" int peer(int x) {
  Guard guard;
  if (x < 0) throw x;
  if (x == 2) throw static_cast<short>(-0x3456);
  return x + 37;
}

int main() {
  if (typed_handler(9) != 46 || !observed(0, 0)) return 1;
  for (int round = 0; round < 2; ++round) {
    // [except.handle] copies the int exception value and matches the short exactly.
    // [except.ctor] destroys the provider's guard before entering either handler.
    reset();
    int value = -0x1234567 - round;
    if (typed_handler(value) != value || !observed(1, 1)) return 2;

    reset();
    try {
      typed_handler(2);
      return 3;
    } catch (short caught) {
      if (caught != -0x3456 || !observed(1, 0)) return 4;
    }
    if (!observed(1, 1)) return 5;
  }
}
