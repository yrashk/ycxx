#include "../profile.hpp"

const unsigned* peer_limit_2() noexcept {
  return &Profile<2u>::limit;
}

const unsigned* peer_limit_4() noexcept {
  return &Profile<4u>::limit;
}
