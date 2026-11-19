// ι (#298): begin() and end() of different types (allowed since C++17): the loop compares the
// iterator with a sentinel through an operator!= taking both, and never compares two iterators
// or two sentinels (`auto __begin = begin-expr; auto __end = end-expr;` are separate
// declarations in [stmt.ranged]/1). A non-member and a member operator!=.
// EXIT: 0
struct End {};

struct CStrIter {
  const char* p;
  char operator*() const { return *p; }
  CStrIter& operator++() {
    ++p;
    return *this;
  }
};

bool operator!=(CStrIter i, End) { return *i.p != 0; }

struct CStr {
  const char* s;
  CStrIter begin() const { return CStrIter{s}; }
  End end() const { return End{}; }
};

struct Limit {
  int n;
};

struct Up {
  int i;
  int operator*() const { return i; }
  Up& operator++() {
    ++i;
    return *this;
  }
  bool operator!=(Limit l) const { return i < l.n; }
};

struct Upto {
  int n;
  Up begin() const { return Up{0}; }
  Limit end() const { return Limit{n}; }
};

int main() {
  CStr word{"hello"};
  int len = 0;
  int sum = 0;
  for (char c : word) {
    ++len;
    sum += c;
  }
  if (len != 5) return 1;
  if (sum != 'h' + 'e' + 'l' + 'l' + 'o') return 2;

  CStr empty{""};
  for (char c : empty) return 3 + c;

  Upto u{4};
  sum = 0;
  for (int i : u) sum += i;
  if (sum != 6) return 4;
  return 0;
}
