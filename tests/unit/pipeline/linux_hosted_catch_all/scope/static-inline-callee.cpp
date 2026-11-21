extern "C" int peer(int);

static inline int forward_to_peer(int x) { return peer(x); }

extern "C" int chosen(int x) {
  try { return forward_to_peer(x); } catch (...) { return 47; }
}
