// δ s3 (#86): automatic objects are destroyed at block exit in the reverse order of their
// construction, observed through a global log ([class.dtor], [stmt.dcl], [basic.life]).
// EXIT: 0
int log_[32];
int n = 0;

struct Noisy {
  int id;
  Noisy(int i) : id(i) { log_[n++] = id; }
  ~Noisy() { log_[n++] = -id; }
};

bool expect(const int* want, int count) {
  if (n != count) return false;
  for (int i = 0; i < count; i++)
    if (log_[i] != want[i]) return false;
  return true;
}

int main() {
  {
    Noisy a(1);
    Noisy b(2);
    {
      Noisy c(3);
    }
    Noisy d(4);
  }
  int want1[] = {1, 2, 3, -3, 4, -4, -2, -1};
  if (!expect(want1, 8)) return 1;

  n = 0;
  for (int i = 1; i <= 2; i++) {
    Noisy x(10 * i);
    Noisy y(10 * i + 1);
  }
  int want2[] = {10, 11, -11, -10, 20, 21, -21, -20};
  if (!expect(want2, 8)) return 2;

  n = 0;
  {
    Noisy arr[3] = {Noisy(5), Noisy(6), Noisy(7)};
  }
  int want3[] = {5, 6, 7, -7, -6, -5};
  if (!expect(want3, 6)) return 3;

  n = 0;
  if (true) {
    Noisy only(8);
  } else {
    Noisy never(9);
  }
  int want4[] = {8, -8};
  if (!expect(want4, 2)) return 4;
  return 0;
}
