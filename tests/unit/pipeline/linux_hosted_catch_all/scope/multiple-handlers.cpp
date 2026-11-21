extern "C" int peer(int);

extern "C" int chosen(int x) {
  try { return peer(x); } catch (int) { return 31; } catch (...) { return 47; }
}
