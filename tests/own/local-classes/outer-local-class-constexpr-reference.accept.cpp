int global = 7;

int witness() {
  constexpr int& local = global;
  struct Local {
    int read() { return local; }
  };
  return Local{}.read();
}
