struct Entry {
  mutable int key;
  int value;
};

constexpr int change_earlier(int& slot) {
  slot = 31;
  return 7;
}

consteval bool write_earlier() {
  const Entry table[3] = {
      {4, 5},
      {((table[0].key = 29), 11), 13},
      {change_earlier(table[1].key), 19},
  };
  return table[0].key == 29 && table[0].value == 5 &&
         table[1].key == 31 && table[1].value == 13 &&
         table[2].key == 7 && table[2].value == 19;
}

static_assert(write_earlier());
