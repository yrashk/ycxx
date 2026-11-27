struct Entry {
  int key;
  int value;
};

consteval int read_dangling(bool inspect) {
  const Entry* alias = nullptr;
  {
    const Entry table[] = {{3, 5}, {11, 17}};
    alias = table + 1;
    if (!inspect)
      return alias->value;
  }
  return alias->value;  //! error[gcc]
}

static_assert(read_dangling(false) == 17);
static_assert(read_dangling(true) == 0);  //! error
