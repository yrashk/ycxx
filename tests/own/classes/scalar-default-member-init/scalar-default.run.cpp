struct Settings {
  int earlier = 9;
  int limit = earlier * 2;
};

int main() {
  Settings settings;
  if (settings.earlier != 9) return 1;
  if (settings.limit != 18) return 2;
}
