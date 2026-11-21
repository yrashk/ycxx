extern "C" int check_left(int);
extern "C" int check_right(int);

int main() {
  if (check_left(7) != 1725) return 1;
  if (check_right(9) != 3945) return 2;
  return 0;
}
