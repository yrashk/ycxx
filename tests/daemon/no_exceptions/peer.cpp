extern "C" int ordinary_cleanup(int input);
extern "C" int ordinary_member_init();
extern "C" void* pinned_try_or_null(bool heapless);

static int event;
static int factory_calls;
static int value = 41;

extern "C" int work(int input) {
  event = 1;
  return input + 2;
}

extern "C" void mark_cleanup() noexcept {
  event = event == 1 ? 2 : -1;
}

extern "C" void* make_value() {
  ++factory_calls;
  return &value;
}

extern "C" int foreign() {
  return 22;
}

int main() {
  if (ordinary_cleanup(4) != 6 || event != 2) return 1;
  if (pinned_try_or_null(true) != nullptr || factory_calls != 0) return 2;
  if (pinned_try_or_null(false) != &value || factory_calls != 1) return 3;
  if (ordinary_member_init() != 22) return 4;
  return 0;
}
