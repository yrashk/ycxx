#include "profile.hpp"

namespace {
struct Tag { static constexpr int number = 29; };
}

const int* peer_limit() noexcept {
  return &Profile<Tag>::limit;
}

int read_limit(const int* address) noexcept {
  return *address;
}
