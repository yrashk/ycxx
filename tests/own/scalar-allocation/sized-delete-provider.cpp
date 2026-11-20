using Size = decltype(sizeof(0));
static long storage;
static int allocated, freed, bad;

void* operator new(Size bytes) {
  ++allocated;
  if (bytes != sizeof(int) || allocated != 1) bad = 1;
  return &storage;
}
void operator delete(void* address) noexcept {
  if (address) bad = 2;
}
void operator delete(void* address, Size bytes) noexcept {
  if (!address) return;
  ++freed;
  if (address != static_cast<void*>(&storage) || bytes != sizeof(int) || freed != 1) bad = 3;
}
int allocation_provider_status() {
  if (bad) return bad;
  return allocated == 1 && freed == 1 ? 0 : 4;
}
