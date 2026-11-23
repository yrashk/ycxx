#include "cross-object.hpp"

Position advance(Position position, long offset) {
  return Position{position.column + offset, position.row - offset};
}

void* operator new(decltype(sizeof(0)), unsigned char* storage) noexcept {
  return storage;
}

int check_explicit_destruction() {
  // Placement construction leaves the single destructor invocation to destroy_position.
  alignas(Position) unsigned char storage[sizeof(Position)];
  Position* position = new (storage) Position{2, 5};
  destroy_position(position);
  return receiver_calls == 1 ? 0 : 3;
}
