namespace {
struct LocalTag { int key; };
LocalTag item{37};
}

const LocalTag& return_only() { return item; }
extern "C" int unaltered_c(const LocalTag& tag) { return tag.key + 1; }

int labelled(const LocalTag&) asm("fixture_preserved_asm_name");
int labelled(const LocalTag& tag) { return tag.key + 2; }

int main() {
  if (return_only().key != 37) return 1;
  if (unaltered_c(item) != 38) return 2;
  return labelled(item) == 39 ? 0 : 3;
}
