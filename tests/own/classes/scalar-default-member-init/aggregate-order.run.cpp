int trace;
int mark(int event, int value) {
  trace = trace * 10 + event;
  return value;
}

struct Settings {
  int earlier;
  int limit = mark(2, earlier * 2);
  int summary = mark(3, earlier + limit);
};

int main() {
  Settings settings{mark(1, 7)};
  if (trace != 123) return 1;
  if (settings.earlier != 7 || settings.limit != 14 || settings.summary != 21) return 2;
}
