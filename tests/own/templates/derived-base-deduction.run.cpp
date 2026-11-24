// A reference or pointer to a base specialization can deduce its template arguments
// from a derived object ([temp.deduct.call]/4.3, #1144).
// EXIT: 0
template<class T> struct Box { T held; };

struct Padding { int pad; };
struct Derived : Padding, Box<int> {};

template<class T> T get(const Box<T>& box) { return box.held; }
template<class T> T get_ptr(const Box<T>* box) { return box->held; }

int main() {
  Derived object{};
  object.pad = 17;
  object.held = 41;
  if (get(object) != 41) return 1;

  const Derived* pointer = &object;
  if (get_ptr(pointer) != 41) return 2;
  return 0;
}
