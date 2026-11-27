struct Local {
  int values[3];
  int following;
};

constexpr int write(int& value) {
  value = 17;
  return 19;
}

consteval int probe() {
  Local local{{7, write(local.values[2]), 11}, 13};
  return local.values[0] + local.values[1] + local.values[2] + local.following;
}

constexpr int observed = probe();  //! error

// XFAIL: gcc accepts this constant evaluation (docs/compatibility.toml, #1140)
