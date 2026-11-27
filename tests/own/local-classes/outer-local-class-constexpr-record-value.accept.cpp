struct Format { int precision; };

int witness() {
  constexpr Format local{24};
  struct Local {
    int read() { return local.precision; }
  };
  return Local{}.read();
}
