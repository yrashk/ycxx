// [temp.expl.spec], [class.dtor]: an explicitly specialized member destructor
// runs for its specialization; a neighboring specialization uses the primary.
// EXIT: 0

int order;

template<class T>
struct Cell {
  ~Cell() { order = order * 10 + 3; }
};

template<> inline Cell<int>::~Cell() { order = order * 10 + 7; }

int main() {
  {
    Cell<int> specialized;
    {
      Cell<char> primary;
    }
    if (order != 3) return 1;
  }
  return order != 37;
}
