extern "C" int excluded_native_throw(int payload) {
  if (payload != 0) throw payload;
  return 2;
}
