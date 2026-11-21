int calls;
int note() { return ++calls; }

struct Settings {
  int earlier;
  int limit = earlier * 2 + note();
};

int main() {
  Settings settings{7, 41};
  if (calls != 0) return 1;
  if (settings.earlier != 7 || settings.limit != 41) return 2;
}
