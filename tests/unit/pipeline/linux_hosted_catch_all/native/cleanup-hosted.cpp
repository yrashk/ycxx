extern "C" int chosen(int);

static int peer_calls;
static int guards_destroyed;
static int exceptions_finished;
static int finished_code;

struct Failure;
static const Failure* constructed;

struct Guard {
  ~Guard() { ++guards_destroyed; }
};

struct Failure {
  int code;

  explicit Failure(int value) : code(value) { constructed = this; }
  Failure(const Failure&) = delete;
  Failure(Failure&&) = delete;

  ~Failure() {
    finished_code = code;
    ++exceptions_finished;
  }
};

extern "C" int peer(int x) {
  ++peer_calls;
  Guard guard;
  if (x < 0) throw Failure{-x};
  return 100 + x;
}

int main() {
  if (chosen(8) != 108) return 1;
  if (peer_calls != 1 || guards_destroyed != 1 || exceptions_finished != 0 || constructed)
    return 2;

  try {
    chosen(-23);
    return 3;
  } catch (const Failure& original) {
    if (&original != constructed || original.code != 23) return 4;
    if (peer_calls != 2 || guards_destroyed != 2 || exceptions_finished != 0)
      return 5;
  } catch (...) {
    return 6;
  }

  return exceptions_finished == 1 && finished_code == 23 && guards_destroyed == 2 ? 0 : 7;
}
