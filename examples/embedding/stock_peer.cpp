#include <stdexcept>

extern "C" void embedded_peer_throw() {
  throw std::logic_error("stock peer exception");
}
