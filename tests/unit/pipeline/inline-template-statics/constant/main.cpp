#include "../profile.hpp"

unsigned peer_limit() noexcept;
static_assert(Profile<2u>::limit == 2u);

int main() {
  return Profile<2u>::limit != peer_limit();
}
