extern "C" void choose_address(int);
extern "C" int call_address(int);

int main() {
  if (call_address(7) != 107) return 1;
  choose_address(1);
  if (call_address(8) != 208) return 2;
  choose_address(0);
  return call_address(9) == 109 ? 0 : 3;
}
