using Count = decltype(sizeof(0));
namespace { struct LocalTag { int token; }; }
static int storage;

void* ordinary_allocate(Count count, const LocalTag& tag) noexcept {
  if (count != sizeof(int)) return nullptr;
  storage = 200 + tag.token;
  return &storage;
}

extern "C" int check_right(int value) {
  LocalTag tag{value};
  void* p = ::ordinary_allocate(sizeof(int), tag);
  return p == &storage ? *static_cast<int*>(p) : -1;
}
