struct Entry {
  int key;
  int value;
};

consteval int read_future_element(bool inspect) {
  const Entry table[2] = {{inspect ? table[1].key : 4, 7}, {11, 13}};  // READ
  return table[0].key;
}

static_assert(read_future_element(false) == 4);
static_assert(read_future_element(true) == 11);  // CALL
