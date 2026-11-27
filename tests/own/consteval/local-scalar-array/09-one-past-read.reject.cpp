struct Local {
  int values[2];
  int following;
};

consteval int probe() {
  Local local{{3, 5}, 11};
  int* end = local.values + 2;
  return *end;  //! error[gcc]
}

constexpr int observed = probe();  //! error[ycxx]  //! error[clang]
