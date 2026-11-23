extern "C" int peer(int);

extern "C" int short_handler(int x) {
  try { return peer(x); }
  catch (short value) { return value; }
}
