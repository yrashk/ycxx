extern "C" int check_left(int value);
extern "C" int check_right(int value);

int main() {
  if (check_left(7) != 107) return 1;
  if (check_right(9) != 209) return 2;
  return 0;
}
