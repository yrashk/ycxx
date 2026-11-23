// [temp.expl.spec], [temp.inst]: completing a class specialization only
// requires the declaration of its non-deleted member destructor.
// EXIT: 0

int observed;

template<class T>
struct Cell {
  int value;
  ~Cell() { observed = 1; }
};

static_assert(sizeof(Cell<int>) == sizeof(int));

template<> Cell<int>::~Cell() { observed = 2; }

int main() {
  { Cell<int> object; }
  return observed != 2;
}
