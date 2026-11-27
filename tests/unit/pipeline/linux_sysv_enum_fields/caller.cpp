#include "enum-abi.hpp"

int main() {
  using namespace abi1682;
  char bytes[5]{};
  char* const address = &bytes[2];

  const auto result = make_result(address, Code::negative);
  if (result.ptr != address || result.ec != Code::negative) return 1;
  if (take_result({address, Code::negative}, cookie, address) != 71) return 2;
  if (take_spilled(11, 22, 33, 44, 55, {address, Code::invalid}, cookie, address) != 73)
    return 3;

  const auto narrow = make_narrow(address, Narrow::value);
  if (narrow.ptr != address || narrow.ec != Narrow::value) return 4;
  if (take_narrow({address, Narrow::value}, cookie, address) != 79) return 5;

  const auto wide = make_wide(address, Wide::value);
  if (wide.ptr != address || wide.ec != Wide::value) return 6;
  if (take_wide({address, Wide::value}, cookie, address) != 83) return 7;

  return 0;
}
