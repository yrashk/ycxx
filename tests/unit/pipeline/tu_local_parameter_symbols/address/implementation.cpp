namespace { struct LocalTag { int key; }; }

int primary(const LocalTag&);
int alternate(const LocalTag&);
using Callback = int (*)(const LocalTag&);
static Callback chosen = &primary;

extern "C" void choose_address(int choice) {
  chosen = choice ? &alternate : &primary;
}

extern "C" int call_address(int key) {
  LocalTag tag{key};
  return chosen(tag);
}

int primary(const LocalTag& tag) { return 100 + tag.key; }
int alternate(const LocalTag& tag) { return 200 + tag.key; }
