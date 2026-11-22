extern "C" int peer(int);

extern "C" int typed_handler(int x) {
  try { return peer(x); }
  catch (int value) { return value; }
}
