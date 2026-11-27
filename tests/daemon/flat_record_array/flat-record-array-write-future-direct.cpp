struct Entry {
  int key;
  int value;
};

consteval int write_future_direct(bool write) {
  Entry table[2] = {
      {write ? ((table[1].value = 29), 3) : 3, 5},  // WRITE
      {11, 17},
  };
  return table[1].value;
}

static_assert(write_future_direct(false) == 17);
static_assert(write_future_direct(true) == 29);  // CALL
