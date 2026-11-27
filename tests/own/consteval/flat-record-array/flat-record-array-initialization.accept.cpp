struct Entry {
  int key;
  int value;
};

consteval bool read_initialized() {
  const Entry table[3] = {
      {4, table[0].key + 3},
      {table[0].value + 2},
  };
  const Entry& first = table[0];
  const Entry* third = table + 2;
  const int* second_value = &table[1].value;
  return first.key == 4 && first.value == 7 &&
         table[1].key == 9 && *second_value == 0 &&
         third->key == 0 && third->value == 0;
}

static_assert(read_initialized());
