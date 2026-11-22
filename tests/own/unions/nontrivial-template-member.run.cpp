// #203: a nontrivial variant of a union template becomes concrete on instantiation.
// EXIT: 0
int constructions;
int destructions;

struct Resource {
  int value;
  explicit Resource(int n) noexcept : value(n) { ++constructions; }
  Resource(const Resource&) = delete;
  ~Resource() noexcept { destructions += value; }
};

template<class Left, class Right>
union Storage {
  Left left;
  Right right;
  Storage(int n) noexcept : left{n} {}
  Storage(long n) noexcept : right{static_cast<int>(n)} {}
  ~Storage() noexcept { destructions += 100; }
};

int main() {
  {
    Storage<Resource, Resource> first(7);
    Storage<Resource, Resource> second(11L);
    if (first.left.value != 7 || second.right.value != 11 || constructions != 2) return 1;
  }
  return destructions != 200;
}
