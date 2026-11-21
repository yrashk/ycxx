enum { runtime_start = 4, runtime_next };

int main() {
  volatile int input = runtime_start;
  return input + runtime_next == 9 ? 0 : 1;
}
