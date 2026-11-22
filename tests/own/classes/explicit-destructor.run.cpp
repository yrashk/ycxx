// #722 [class.dtor]: direct calls end a member's lifetime without releasing storage.
// PEER: explicit-destructor-peer.cpp
// EXIT: 0
struct Resource { int value; ~Resource() noexcept; };
struct Trivial { int value; };

int destroyed, receiver_calls;
Resource::~Resource() noexcept { destroyed += value; }

Resource& once(Resource& object) {
  ++receiver_calls;
  return object;
}

void destroy_member(Resource& object) {
  once(object).~Resource();
}

void destroy_const_member(const Resource* pointer) {
  using Alias = Resource;
  pointer->~Alias();
}

Trivial* once(Trivial* pointer) {
  ++receiver_calls;
  return pointer;
}

void destroy_trivial_member(Trivial* pointer) {
  once(pointer)->~Trivial();
}

int peer();
int main() { return peer(); }
