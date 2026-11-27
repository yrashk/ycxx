struct Entry {
  int key;
  int value;
};

consteval int read_future_member(bool inspect) {
  const Entry table[2] = {{inspect ? table[0].value : 4, 7}, {11, 13}};  // READ
  return table[0].key;
}

static_assert(read_future_member(false) == 4);
static_assert(read_future_member(true) == 7);  // CALL
