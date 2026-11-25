// EXIT: 0

unsigned long trace = 0;

struct Empty {
  Empty() { trace = trace * 10 + 3; }
};

static_assert(sizeof(Empty) == 1 && alignof(Empty) == 1);

int first_argument() {
  trace = trace * 10 + 1;
  return 11;
}

Empty make(int a, int b, int c, int d, int e, int f, int g) {
  const bool correct = a == 11 && b == 22 && c == 33 && d == 44 &&
                       e == 55 && f == 66 && g == 77;
  trace = trace * 10 + (correct ? 2 : 9);
  return {};
}

int main() {
  Empty value = make(first_argument(), 22, 33, 44, 55, 66, 77);
  const Empty& bound = make(first_argument(), 22, 33, 44, 55, 66, 77);
  make(first_argument(), 22, 33, 44, 55, 66, 77);

  if (&value == &bound)
    return 1;
  return trace == 123123123UL ? 0 : 2;
}
