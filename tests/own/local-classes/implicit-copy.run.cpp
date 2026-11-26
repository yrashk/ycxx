// Local classes (#117), spec-only: a local class gets an implicitly-declared copy constructor and
// copy assignment operator that copy every member, including arrays; a user-provided copy
// constructor of a member's local class is used by them ([class.copy.ctor], [class.copy.assign]).
int main() {
  struct Counted {
    int* copies;
    int v;
    Counted(int* c, int x) : copies(c), v(x) {}
    Counted(const Counted& o) : copies(o.copies), v(o.v) { ++*copies; }
    Counted& operator=(const Counted& o) {
      v = o.v;
      ++*copies;
      return *this;
    }
  };
  struct Box {
    int arr[3];
    Counted c;
  };
  int copies = 0;
  Box a{{1, 2, 3}, Counted(&copies, 4)};
  int before = copies;
  Box b = a;
  if (copies != before + 1) return 1;
  if (b.arr[0] != 1 || b.arr[2] != 3 || b.c.v != 4) return 2;
  b.arr[1] = 20;
  if (a.arr[1] != 2) return 3;
  Box c{{0, 0, 0}, Counted(&copies, 0)};
  int mid = copies;
  c = b;
  if (copies != mid + 1) return 4;
  if (c.arr[1] != 20 || c.c.v != 4) return 5;
  return 0;
}
