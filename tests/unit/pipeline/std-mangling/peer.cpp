#include <cstddef>

std::byte combine(std::byte, std::byte);

int main(int argc, char**) {
  auto value = static_cast<std::byte>(argc);
  auto bits = static_cast<std::byte>(3);
  auto host = value | bits;
  auto guest = combine(value, bits);
  return host == guest && host == static_cast<std::byte>(argc | 3) ? 0 : 1;
}
