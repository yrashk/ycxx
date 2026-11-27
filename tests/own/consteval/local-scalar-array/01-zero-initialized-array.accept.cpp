struct Local {
  int values[3];
  int following;
};

consteval bool probe() {
  Local local{};
  const int* first = local.values;
  return first[0] == 0 && first[1] == 0 && first[2] == 0 && local.following == 0;
}

static_assert(probe());
