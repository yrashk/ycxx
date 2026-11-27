struct Local {
  int values[3];
  int following;
};

constexpr int read(const int& value) { return value; }

consteval int probe() {
  Local local{{7, read(local.values[2]), 11}, 13};
  return local.values[1];
}

constexpr int observed = probe();  //! error

// XFAIL: gcc accepts this constant evaluation (docs/compatibility.toml, #1140)
