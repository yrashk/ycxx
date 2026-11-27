struct Entry {
  int key;
  int value;
};

constexpr int write_slot(int& slot) {
  slot = 29;  // WRITE
  return 3;
}

consteval int write_future_helper(bool write) {
  Entry table[2] = {
      {write ? write_slot(table[1].value) : 3, 5},  // BIND
      {11, 17},
  };
  return table[1].value;
}

static_assert(write_future_helper(false) == 17);
static_assert(write_future_helper(true) == 29);  // CALL
