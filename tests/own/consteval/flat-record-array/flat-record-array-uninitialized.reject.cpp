struct Entry {
  int key;
  int value;
};

consteval int read_uninitialized(bool inspect) {
  Entry table[2];
  if (inspect)
    return table[1].value;  // READ
  return 17;
}

static_assert(read_uninitialized(false) == 17);
static_assert(read_uninitialized(true) == 0);  // CALL
