int trace;
int mark(int event, int value) {
  trace = trace * 10 + event;
  return value;
}

struct Settings {
  int earlier;
  int limit = mark(2, earlier * 2);
  int summary = mark(3, earlier + limit);

  Settings() : earlier(mark(1, 7)) {}
  Settings(int replacement)
      : summary(mark(3, earlier + limit)), limit(mark(4, replacement)), earlier(mark(1, 7)) {}
};

int main() {
  Settings omitted;
  if (trace != 123 || omitted.limit != 14 || omitted.summary != 21) return 1;
  trace = 0;
  Settings overridden(41);
  if (trace != 143 || overridden.limit != 41 || overridden.summary != 48) return 2;
}
