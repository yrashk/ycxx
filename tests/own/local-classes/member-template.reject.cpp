// Local classes (#117), spec-only: a local class cannot have member templates ([temp.mem],
// [class.local]).
int main() {
  struct L {
    template <class T> T get() { return T(); } //! error
  };
  return 0;
}
