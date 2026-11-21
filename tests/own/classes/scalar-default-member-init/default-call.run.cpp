int calls;
int next_value() { return 12 + ++calls; }

struct Settings { int limit = next_value(); };

int main() {
  Settings first;
  Settings second;
  if (calls != 2) return 1;
  if (first.limit != 13 || second.limit != 14) return 2;
}
