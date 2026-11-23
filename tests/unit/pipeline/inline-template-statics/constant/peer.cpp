#include "../profile.hpp"

unsigned peer_limit() noexcept {
  return Profile<2u>::limit;
}
