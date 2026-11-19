// ι (#298): nested range-based for loops over different kinds of range (an array of strings
// as arrays of char, a class range inside an array loop), the inner loop's range depending on the
// outer loop variable, each loop with its own `__range`, `__begin` and `__end`; the output is
// written with putchar ([stmt.ranged]/1).
// EXIT: 0
// STDOUT: ab-ab-ab
// STDOUT: xyz
// STDOUT: 1 22 333
extern "C" int putchar(int);

struct Repeat {
  int digit;
  int times;
  struct It {
    int d;
    int left;
    int operator*() const { return d; }
    It& operator++() {
      --left;
      return *this;
    }
    bool operator!=(const It& o) const { return left != o.left; }
  };
  It begin() const { return It{digit, times}; }
  It end() const { return It{digit, 0}; }
};

int main() {
  char words[3][3] = {"ab", "ab", "ab"};
  int first = 1;
  for (auto& w : words) {
    if (!first) putchar('-');
    first = 0;
    for (char c : w)
      if (c != 0) putchar(c);
  }
  putchar('\n');

  char rows[1][4] = {"xyz"};
  for (auto& r : rows)
    for (char c : r)
      if (c) putchar(c);
  putchar('\n');

  int ns[3] = {1, 2, 3};
  int count = 0;
  for (int n : ns) {
    if (n != 1) putchar(' ');
    for (int d : Repeat{n, n}) {
      putchar('0' + d);
      ++count;
    }
  }
  putchar('\n');
  return count == 6 ? 0 : 1;
}
