// No-throw comparison for per-target object generation.
extern "C" int authored_normal_only(int value) {
  if (value == 0) return 41;
  return value;
}
