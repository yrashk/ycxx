// ι (#94), spec-only: variables in named, nested and unnamed namespaces, and in a reopened
// namespace, are dynamically initialized in the order of their definitions in the translation
// unit, interleaved with each other, and the class objects among them are destroyed in reverse
// ([basic.start.dynamic], [basic.start.term], [namespace.def], [namespace.unnamed]).
// STDOUT: 12345
// STDOUT: M
// STDOUT: 531
// EXIT: 0
extern "C" int putchar(int);

int count;

int next(int c) {
  putchar(c);
  return ++count;
}

struct Tag {
  int c;
  Tag(int x) : c(x) { putchar(x); }
  ~Tag() {
    putchar(c);
    if (c == '1') putchar('\n');
  }
};

namespace outer {
  Tag t1('1');
  int v2 = next('2');
  namespace inner {
    Tag t3('3');
  }
}

namespace {
  int v4 = next('4');
}

namespace outer {
  Tag t5('5');
  int reads = v2 + inner::t3.c;
}

int main() {
  putchar('\n');
  putchar('M');
  putchar('\n');
  if (outer::v2 != 1) return 1;
  if (v4 != 2) return 2;
  if (outer::reads != 1 + '3') return 3;
  if (outer::inner::t3.c != '3') return 4;
  if (outer::t5.c != '5') return 5;
  return 0;
}
