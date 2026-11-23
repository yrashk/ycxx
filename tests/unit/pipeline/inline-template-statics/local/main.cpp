#include "profile.hpp"

namespace {
struct Tag { static constexpr int number = 11; };
}

const int* peer_limit() noexcept;
int read_limit(const int* address) noexcept;

// [basic.link]: the two unnamed-namespace Tags denote different types.
// Each specialization therefore has its own initialized member storage.
int main() {
  const int* own = &Profile<Tag>::limit;
  const int* peer = peer_limit();
  if (own == peer) return 1;
  return read_limit(own) != 11 || read_limit(peer) != 29;
}
