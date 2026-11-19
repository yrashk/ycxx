// ι (#94), spec-only: zero-initialization of every static-storage variable happens before any
// dynamic initialization ([basic.start.static]), so an initializer that reads a variable defined
// later, whose own dynamic initialization has not happened yet, sees zero; that variable is
// initialized afterwards, in its turn ([basic.start.dynamic]). The later initializers change
// another static object (`effects`), so the implementation may not initialize them early instead
// ([basic.start.static]/3), which would make the value read unspecified.
// EXIT: 0
int effects;

int f() {
  ++effects;
  return 7;
}

int* note(int* p) {
  ++effects;
  return p;
}

int later_value();
int* later_pointer();

int seen = later_value() + 1;
int* seen_pointer = later_pointer();
int effects_seen = effects;

int later = f();
int* pointer = note(&later);

int later_value() { return later; }
int* later_pointer() { return pointer; }

int main() {
  if (seen != 1) return 1;
  if (seen_pointer != 0) return 2;
  if (effects_seen != 0) return 3;
  if (later != 7) return 4;
  if (pointer != &later) return 5;
  if (later_value() != 7) return 6;
  if (effects != 2) return 7;
  return 0;
}
