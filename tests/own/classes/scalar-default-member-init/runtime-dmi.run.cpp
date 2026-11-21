constexpr int mode() {
  if consteval { return 1; }
  else { return 2; }
}

static_assert(mode() == 1);

struct X { int value = mode(); };

int main() {
  X default_initialized;
  if (default_initialized.value != 2) return 1;
  X aggregate_initialized{};
  if (aggregate_initialized.value != 2) return 2;
}
