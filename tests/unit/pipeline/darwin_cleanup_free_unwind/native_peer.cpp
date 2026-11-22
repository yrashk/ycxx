extern "C" int bridge(int);
extern "C" int scalar_helper(int);
extern "C" int callback_helper(int);

extern "C" int peer(int value) {
  return value * 3;
}

int main() {
  if (bridge(0) != 43 || bridge(8) != 29) return 1;
  if (scalar_helper(5) != 17 || callback_helper(9) != 24) return 2;
  return bridge(6) == 23 ? 0 : 3;
}
