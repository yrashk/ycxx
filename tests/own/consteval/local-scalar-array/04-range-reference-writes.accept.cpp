struct Local {
  int values[3];
  int following;
};

consteval bool probe() {
  Local local{{1, 3, 5}, 11};
  for (int& value : local.values) value += 2;
  return local.values[0] == 3 && local.values[1] == 5 && local.values[2] == 7 && local.following == 11;
}

static_assert(probe());
