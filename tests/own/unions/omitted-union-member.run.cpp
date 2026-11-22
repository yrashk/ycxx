// #203 [dcl.init.aggr]: a union element implicitly initialized from {} initializes
// its first variant when no variant has a default member initializer.
// XFAIL: gcc GCC 16.2 leaves the first variant uninitialized under the pinned C++26 rule.
// EXIT: 0
int constructors;
struct Resource {
  int value;
  Resource() noexcept : value(7) { ++constructors; }
  Resource(const Resource&) = delete;
  ~Resource() noexcept {}
};
union Storage {
  Resource first;
  int fallback;
  ~Storage() noexcept {}
};
struct Holder { Storage storage; };
int main() {
  Holder holder{};
  return constructors != 1 || holder.storage.first.value != 7;
}
