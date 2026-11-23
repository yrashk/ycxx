// [class.static.data], [dcl.init.list], [dcl.init.aggr], [basic.scope.pdecl]
// EXIT: 0

int target = 9;
char width;

struct Store {
  static constexpr int seed = 41;
  inline static int direct{seed + 1};
  inline static int copied = {17};
  inline static int width{sizeof(width)};
  inline static int cells[3]{seed, 22};
  inline static int copied_cells[2] = {17, 22};
  inline static int empty{};
  inline static int empty_cells[2] = {};
  inline static int unwritten[2];
  inline static int assigned = 23;
  inline static int* pointers[2] = {&target};
  inline static double scale{1.25};
  static constexpr int constant = {7};
};

static_assert(Store::constant == 7);

int main() {
  if (Store::direct != 42 || Store::copied != 17 || Store::width != sizeof(int)) return 1;
  if (Store::cells[0] != 41 || Store::cells[1] != 22 || Store::cells[2] != 0) return 2;
  if (Store::copied_cells[0] != 17 || Store::copied_cells[1] != 22) return 3;
  if (Store::empty != 0 || Store::empty_cells[0] != 0 || Store::empty_cells[1] != 0) return 4;
  if (Store::unwritten[0] != 0 || Store::unwritten[1] != 0 || Store::assigned != 23) return 5;
  if (Store::pointers[0] != &target || Store::pointers[1] != nullptr || Store::scale != 1.25) return 6;
  int* storage = &Store::direct;
  *storage += *Store::pointers[0];
  return Store::direct != 51;
}
