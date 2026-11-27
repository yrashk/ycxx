struct Table { unsigned long long entries[2]; };

consteval unsigned long long read_entry() {
  Table value{};
  value.entries[1] = 5;
  return value.entries[1];
}

static_assert(read_entry() == 5);
