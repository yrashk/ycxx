// δ s3 (#86): members are initialized in declaration order whatever the order of the
// mem-initializer list, and destroyed after the destructor body in reverse declaration order;
// a member without a mem-initializer uses its default member initializer or its default
// constructor ([class.base.init], [class.dtor]).
// EXIT: 0
int log_[32];
int n = 0;

struct Part {
  int id;
  Part() : id(9) { log_[n++] = id; }
  Part(int i) : id(i) { log_[n++] = id; }
  ~Part() { log_[n++] = -id; }
};

struct Machine {
  Part first;
  Part second;
  Part third = Part(3);
  Part fourth;
  Machine() : second(2), first(1) { log_[n++] = 100; }  // still first, second, third, fourth
  ~Machine() { log_[n++] = -100; }
};

bool expect(const int* want, int count) {
  if (n != count) return false;
  for (int i = 0; i < count; i++)
    if (log_[i] != want[i]) return false;
  return true;
}

int main() {
  {
    Machine m;
    int ctor[] = {1, 2, 3, 9, 100};
    if (!expect(ctor, 5)) return 1;
    if (m.fourth.id != 9) return 2;
  }
  int all[] = {1, 2, 3, 9, 100, -100, -9, -3, -2, -1};
  if (!expect(all, 10)) return 3;
  return 0;
}
