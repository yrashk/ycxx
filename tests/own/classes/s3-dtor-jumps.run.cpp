// δ s3 (#86): leaving nested scopes by `return`, `break` and `continue` destroys every automatic
// object constructed in the scopes left, in reverse order, and none other
// ([stmt.jump.general], [stmt.return], [stmt.break], [stmt.cont], [class.dtor]).
// EXIT: 0
int log_[64];
int n = 0;

struct Guard {
  int id;
  Guard(int i) : id(i) { log_[n++] = id; }
  ~Guard() { log_[n++] = -id; }
};

bool expect(const int* want, int count) {
  if (n != count) return false;
  for (int i = 0; i < count; i++)
    if (log_[i] != want[i]) return false;
  return true;
}

int early(bool leave) {
  Guard a(1);
  {
    Guard b(2);
    if (leave) {
      Guard c(3);
      return 7;
    }
    Guard d(4);
  }
  return 8;
}

int main() {
  if (early(true) != 7) return 1;
  int want1[] = {1, 2, 3, -3, -2, -1};
  if (!expect(want1, 6)) return 2;

  n = 0;
  if (early(false) != 8) return 3;
  int want2[] = {1, 2, 4, -4, -2, -1};
  if (!expect(want2, 6)) return 4;

  n = 0;
  {
    Guard outer(100);
    for (int i = 1; i <= 3; i++) {
      Guard body(i * 10);
      if (i == 1) {
        Guard skip(11);
        continue;
      }
      Guard rest(i * 10 + 2);
      if (i == 2) {
        Guard stop(23);
        break;
      }
      Guard never(99);
    }
  }
  int want3[] = {100, 10, 11, -11, -10, 20, 22, 23, -23, -22, -20, -100};
  if (!expect(want3, 12)) return 5;

  n = 0;
  int k = 0;
  while (true) {
    Guard w(50 + k);
    do {
      Guard inner(60 + k);
      if (++k == 2) break;
    } while (false);
    if (k == 2) break;
  }
  int want4[] = {50, 60, -60, -50, 51, 61, -61, -51};
  if (!expect(want4, 8)) return 6;
  return 0;
}
