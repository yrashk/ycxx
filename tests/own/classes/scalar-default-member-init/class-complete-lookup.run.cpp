int scale(int number) { return number + 50; }

struct Settings {
  int earlier;
  int limit = scale(earlier);
  static int scale(int number) { return number * 2; }
};

int main() {
  Settings settings{7};
  if (::scale(7) != 57) return 1;
  if (settings.limit != 14) return 2;
}
