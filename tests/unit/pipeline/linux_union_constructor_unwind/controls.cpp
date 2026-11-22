extern int initialize_payload();
extern void after_initialize();
int owner_destroyed;
int union_destroyed;
struct Resource {
  int payload;
  ~Resource() noexcept { owner_destroyed += payload; }
};
union Storage {
  Resource owner;
  int fallback;
  Storage() : owner{initialize_payload()} { after_initialize(); }
  ~Storage() noexcept { ++union_destroyed; }
};
void probe_controls() { Storage object; }
