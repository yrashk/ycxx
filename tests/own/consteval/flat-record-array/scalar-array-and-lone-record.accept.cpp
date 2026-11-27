struct Entry {
  int key;
  int value;
};

consteval int read_scalar_array() {
  const int values[] = {3, 7};
  return values[0] * values[1];
}

consteval int read_lone_record() {
  const Entry entry = {9, 5};
  return entry.key + entry.value;
}

static_assert(read_scalar_array() == 21);
static_assert(read_lone_record() == 14);
