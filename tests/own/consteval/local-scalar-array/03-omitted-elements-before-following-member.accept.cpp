struct Local {
  int values[3];
  int following;
};

consteval bool probe() {
  Local local{{7}, local.values[1] + local.values[2] + 9};
  return local.values[0] == 7 && local.values[1] == 0 && local.values[2] == 0 && local.following == 9;
}

static_assert(probe());
