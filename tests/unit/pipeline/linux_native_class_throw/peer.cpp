#include "types.hpp"

static_assert(sizeof(Decoy) == sizeof(Fault));
static_assert(alignof(Decoy) == alignof(Fault));

int check_fault(int input) {
  try {
    const int result = produce(input);
    return input == 0 && result == 19 ? 0 : 1;
  } catch (const Decoy&) {
    return 2;
  } catch (const Fault& fault) {
    return input != 0 && fault.payload == input ? 0 : 3;
  } catch (...) {
    return 4;
  }
}

int main() {
  if (const int result = check_fault(0))
    return result;
  if (const int result = check_fault(0x12345678))
    return 10 + result;
  if (const int result = check_fault(-0x23456789))
    return 20 + result;

  try {
    produce_int(-12345);
  } catch (int value) {
    return value == -12345 ? 0 : 31;
  } catch (...) {
    return 32;
  }
  return 30;
}
