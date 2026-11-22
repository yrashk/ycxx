extern "C" int peer(int);

extern "C" int bridge(int value) {
  if (value == 0) return 43;
  return peer(value) + 5;
}

extern "C" int scalar_helper(int value) {
  return value + 12;
}

extern "C" int callback_helper(int value) {
  return peer(value) - 3;
}
