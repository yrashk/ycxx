struct Local {
  int values[2];
  int following;
};

consteval bool probe() {
  Local local{{3, 5}, 11};
  int* first = local.values;
  int* end = first + 2;
  return end - first == 2 && *(end - 1) == 5 && local.following == 11;
}

static_assert(probe());
