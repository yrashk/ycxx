struct Entry {
  int key;
  int value;
};

consteval int write_current(bool write) {
  Entry table[2] = {
      {write ? ((table[0].key = 29), 3) : 3, 5},  // WRITE
      {11, 17},
  };
  return table[0].key;
}

static_assert(write_current(false) == 3);
static_assert(write_current(true) == 3);  // CALL
