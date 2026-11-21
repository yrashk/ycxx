// EXIT: 0
namespace fixture {
struct Box {
  int value;

  int& select() & { return value; }
  int select() && { return value + 20; }
  int read() & { return this->select(); }
};
}

int main() {
  fixture::Box box{4};
  box.select() = 7;
  fixture::Box&& named = static_cast<fixture::Box&&>(box);
  if (named.select() != 7) return 1;
  if ((&box)->read() != 7) return 2;
  return 0;
}
