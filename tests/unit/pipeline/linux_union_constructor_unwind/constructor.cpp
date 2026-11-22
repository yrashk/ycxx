extern void external_throw();
int destroyed;
struct Resource {
  int value;
  ~Resource() noexcept { destroyed += value; }
};
union Storage {
  Resource owner;
  int fallback;
  Storage() : owner{7} { external_throw(); }
  ~Storage() noexcept {}
};
void probe() { Storage object; }
