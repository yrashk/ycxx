enum class Code : unsigned { first = 3, second = 5 };

consteval int read_range() {
  const struct {
    Code code;
    int value;
  } entries[] = {{Code::first, 2}, {Code::second, 7}};

  int result = 0;
  for (const auto& entry : entries) {
    if (entry.code == Code::first)
      result = result * 10 + entry.value;
    else if (entry.code == Code::second)
      result = result * 10 + entry.value + 1;
    else
      return -1;
  }
  return result;
}

static_assert(read_range() == 28);
