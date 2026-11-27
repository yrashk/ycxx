struct Local {
  int values[3];
  int following;
};

constexpr int ignore(const int&) { return 19; }

consteval bool probe() {
  Local local{{7, ignore(local.values[2]), 11}, 13};
  return local.values[1] == 19 && local.values[2] == 11;
}

static_assert(probe());
