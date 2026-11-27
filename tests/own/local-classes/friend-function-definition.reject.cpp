// Local classes (#117), spec-only: a function can be defined in a friend declaration only if the
// class is not local ([class.friend], [class.local]).
int main() {
  struct L {
    int v;
    friend int peek(L l) { return l.v; } //! error
  };
  return 0;
}
