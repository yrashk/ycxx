extern "C" int peer(int);

extern "C" int transit(int x) {
  return peer(x) + 3;
}

extern "C" int chosen(int x) {
  try { return peer(x); }
  catch (...) { return 47; }
}
