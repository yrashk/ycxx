struct Entry {
  int key;
  int value;
};

consteval int read_named() {
  const Entry table[] = {{2, 3}, {7, 11}};
  return table[0].key + table[1].key * table[1].value;
}

static_assert(read_named() == 79);
