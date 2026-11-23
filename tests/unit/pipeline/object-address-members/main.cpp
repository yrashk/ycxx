#include "probe.hpp"

namespace config {
int shared = 17;
template<> int Probe<&shared>::read() noexcept { return shared; }
}
using config::Probe;
using config::shared;
static int private_value = 31;

int main() {
  Read shared_here = &Probe<&shared>::read;
  Read local_here = &Probe<&private_value>::read;
  Read shared_there = peer_shared();
  Read local_there = peer_private();
  if (&shared != peer_shared_object() || shared_here != shared_there) return 1;
  if (&private_value == peer_private_object() || local_here == local_there) return 2;
  if (shared_here() != 17 || shared_there() != 17) return 3;
  if (local_here() != 31 || local_there() != 47) return 4;
  shared = 59;
  private_value = 67;
  if (shared_there() != 59 || local_here() != 67 || local_there() != 47) return 5;
  return 0;
}
