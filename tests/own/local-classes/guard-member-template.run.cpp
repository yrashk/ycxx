// #117: the shape of libycxx's basic_string::resize_and_overwrite: in a member function template of
// a class template, a local scope guard with reference members, a default member initializer and a
// constexpr destructor that calls a private static member of the enclosing class and uses its
// template parameter; dismissed on success, run at scope exit otherwise ([class.local],
// [class.dtor], [temp.inst], [class.access.general]).
template <class Alloc> class Buffer {
  static void release(Alloc& a, int& block) {
    a.released += block;
    block = 0;
  }
  Alloc alloc_;
  int size_;

public:
  Buffer() : alloc_{0}, size_(0) {}
  template <class Op> bool grow(int block, Op op) {
    struct Guard {
      Alloc& a;
      int& b;
      bool active = true;
      constexpr ~Guard() {
        if (active) release(a, b);
      }
    } guard{alloc_, block};
    if (!op(block)) return false;  // the guard releases the block
    guard.active = false;
    size_ += block;
    return true;
  }
  int size() const { return size_; }
  int released() const { return alloc_.released; }
};

struct Counting {
  int released;
};

int main() {
  Buffer<Counting> b;
  if (!b.grow(8, [](int n) { return n > 0; })) return 1;
  if (b.size() != 8 || b.released() != 0) return 2;
  if (b.grow(5, [](int) { return false; })) return 3;
  if (b.size() != 8 || b.released() != 5) return 4;
  // A second specialization of the member template has its own local class.
  if (!b.grow(2, [](long) { return true; }) || b.size() != 10) return 5;
  return 0;
}
