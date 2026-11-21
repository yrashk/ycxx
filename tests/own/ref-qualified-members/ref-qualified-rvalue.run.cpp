// EXIT: 0
namespace fixture {
struct Box {
  int value;

  int select() &;
  int select() &&;
  int read_lvalue() & { return value; }
};
int Box::select() && { return this->read_lvalue() + 20; }
int Box::select() & { return value + 10; }

Box&& as_rvalue(Box& box) { return static_cast<Box&&>(box); }
}

int main() {
  fixture::Box box{4};
  if (box.select() != 14) return 1;
  if (static_cast<fixture::Box&&>(box).select() != 24) return 2;
  if (fixture::Box{5}.select() != 25) return 3;
  if (fixture::as_rvalue(box).select() != 24) return 4;
  return 0;
}
