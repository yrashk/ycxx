// δ s3 (#86): overloaded constructors chosen by exact match and by integral promotion over
// integral conversion, and by the number of arguments
// ([over.match.ctor], [over.ics.rank], [conv.prom]).
// EXIT: 0
struct Tag {
  int which;
  Tag() : which(0) {}
  Tag(int) : which(1) {}
  Tag(long) : which(2) {}
  Tag(unsigned) : which(3) {}
  Tag(int*) : which(4) {}
  Tag(int, int) : which(5) {}
  Tag(const char*) : which(6) {}
};

int main() {
  int x = 0;
  short s = 1;
  char c = 'c';
  bool b = true;
  long l = 1;
  unsigned u = 1;
  Tag t0;
  if (t0.which != 0) return 1;
  if (Tag(x).which != 1) return 2;
  if (Tag(s).which != 1) return 3;  // short promotes to int
  if (Tag(c).which != 1) return 4;  // char promotes to int
  if (Tag(b).which != 1) return 5;  // bool promotes to int
  if (Tag(l).which != 2) return 6;
  if (Tag(u).which != 3) return 7;
  if (Tag(&x).which != 4) return 8;
  if (Tag(1, 2).which != 5) return 9;
  if (Tag("hi").which != 6) return 10;
  Tag t{7L};
  if (t.which != 2) return 11;
  Tag v = 3u;
  if (v.which != 3) return 12;
  return 0;
}
