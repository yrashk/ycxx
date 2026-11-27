struct Local {
  int values[3];
  int following;
};

constexpr int read(const int& value) { return value; }

consteval int probe() {
  Local local{{7, read(local.values[0]), 11}, 13};
  return local.values[1];
}

static_assert(probe() == 7);
