struct Entry {
  int key;
  int value;
};

consteval int read_past_end(bool inspect) {
  const Entry table[] = {{3, 5}, {11, 17}};
  const Entry* end = table + 2;
  if (inspect)
    return end->value;  // READ
  return (end - 1)->value;
}

static_assert(read_past_end(false) == 17);
static_assert(read_past_end(true) == 0);  // CALL
