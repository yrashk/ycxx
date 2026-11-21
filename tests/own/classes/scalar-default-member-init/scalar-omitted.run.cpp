struct Settings {
  int earlier = 11;
  int limit = earlier * 2;
};

int limit_for(int seed) {
  Settings settings{seed};
  return settings.limit;
}

int main() {
  if (limit_for(7) != 14) return 1;
  if (limit_for(-4) != -8) return 2;
  Settings settings{};
  if (settings.earlier != 11 || settings.limit != 22) return 3;
}
