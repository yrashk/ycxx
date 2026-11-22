extern "C" int peer(int);

extern "C" int call_in_handler(int x) {
  try { return peer(x); }
  catch (...) { return peer(1); }
}
