extern "C" int bridge(int);
extern "C" int scalar_helper(int);
extern "C" int callback_helper(int);

extern "C" int peer(int value) {
  if (value < 0) throw value;
  return value * 3;
}

int main() {
  if (bridge(0) != 43) return 1;
  if (bridge(8) != 29 || scalar_helper(5) != 17 || callback_helper(9) != 24) return 2;
  try {
    bridge(-19);
    return 3;
  } catch (int value) {
    if (value != -19) return 4;
  }
  return bridge(6) == 23 ? 0 : 5;
}
