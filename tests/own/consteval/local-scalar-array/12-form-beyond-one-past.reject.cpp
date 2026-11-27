struct Local {
  int values[2];
  int following;
};

consteval int probe() {
  Local local{{3, 5}, 11};
  int* beyond = local.values + 3;
  return *(beyond - 2);
}

constexpr int observed = probe();  //! error

// XFAIL: gcc accepts this constant evaluation (docs/compatibility.toml, #1140)
