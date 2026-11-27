struct Local {
  int values[2];
  int following;
};

constexpr void write(int* destination) { *destination = 19; }  //! error[gcc]

consteval int probe() {
  Local local{{3, 5}, 11};
  int* end = local.values + 2;
  write(end);
  return local.following;
}

constexpr int observed = probe();  //! error[ycxx]  //! error[clang]
