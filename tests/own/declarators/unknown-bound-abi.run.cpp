// An omitted major bound is encoded in a function parameter type (Itanium C++ ABI 5.1.5.6, #222).
extern int unknown_rows[][3];
int read_unknown_rows(int (*pointer)[][3]) {
  return (*pointer)[0][1] + (*pointer)[1][2];
}
int main() { return read_unknown_rows(&unknown_rows) == 42 ? 0 : 1; }
int unknown_rows[2][3] = {{1, 9, 4}, {5, 6, 33}};
