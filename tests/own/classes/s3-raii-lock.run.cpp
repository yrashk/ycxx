// δ s3 (#86): RAII: a scope guard that takes a resource in its constructor and gives it back in
// its destructor, through a reference member, on every path out of a function
// ([class.ctor], [class.dtor], [stmt.return], [dcl.ref]).
// EXIT: 0
struct Mutex {
  int held;
  int max_held;
  int acquisitions;
};

class Lock {
  Mutex& m_;

public:
  Lock(Mutex& m) : m_(m) {
    ++m_.held;
    ++m_.acquisitions;
    if (m_.held > m_.max_held) m_.max_held = m_.held;
  }
  ~Lock() { --m_.held; }
};

Mutex mu;
int shared_value = 0;

int update(int v) {
  Lock guard(mu);
  if (v < 0) return -1;
  {
    Lock nested(mu);
    if (v == 0) return 0;
    shared_value += v;
  }
  for (int i = 0; i < v; i++) {
    Lock step(mu);
    if (i == 2) break;
  }
  return shared_value;
}

int main() {
  if (update(-5) != -1) return 1;
  if (mu.held != 0) return 2;
  if (update(0) != 0) return 3;
  if (mu.held != 0) return 4;
  if (update(4) != 4) return 5;
  if (mu.held != 0) return 6;
  if (mu.max_held != 2) return 7;
  if (mu.acquisitions != 1 + 2 + 2 + 3) return 8;
  return 0;
}
