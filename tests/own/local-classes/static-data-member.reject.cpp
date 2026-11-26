// Local classes (#117), spec-only: a local class cannot have static data members
// ([class.static.data], [class.local]).
int main() {
  struct L {
    static int count; //! error
    int v;
  };
  return L{0}.v;
}
