#include "enum-abi.hpp"

namespace abi1682 {

Result make_result(char* address, Code code) {
  return {address, code};
}

int take_result(Result input, unsigned long long tail, char* expected) {
  return input.ptr == expected && input.ec == Code::negative && tail == cookie ? 71 : 0;
}

int take_spilled(unsigned long long a, unsigned long long b, unsigned long long c,
                 unsigned long long d, unsigned long long e, Result input,
                 unsigned long long tail, char* expected) {
  return a == 11 && b == 22 && c == 33 && d == 44 && e == 55 &&
         input.ptr == expected && input.ec == Code::invalid && tail == cookie ? 73 : 0;
}

NarrowResult make_narrow(char* address, Narrow code) {
  return {address, code};
}

int take_narrow(NarrowResult input, unsigned long long tail, char* expected) {
  return input.ptr == expected && input.ec == Narrow::value && tail == cookie ? 79 : 0;
}

WideResult make_wide(char* address, Wide code) {
  return {address, code};
}

int take_wide(WideResult input, unsigned long long tail, char* expected) {
  return input.ptr == expected && input.ec == Wide::value && tail == cookie ? 83 : 0;
}

}
