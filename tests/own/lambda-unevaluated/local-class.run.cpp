// A lambda in `decltype` in a member alias of a local class (#1549): its closure type is in the
// class, in the function's <local-name>, and each function's is its own.
template <class F>
int call() {
  return F{}();
}
int f() {
  struct L {
    using c = decltype([] { return 1; });
  };
  return call<L::c>();
}
int g() {
  struct L {
    using c = decltype([] { return 2; });
  };
  return call<L::c>();
}
template <class T>
int t() {
  struct L {
    using c = decltype([] { return int(sizeof(T)); });
  };
  return call<typename L::c>();
}
int main() { return f() + g() + t<long>() + t<char>() == 12 ? 0 : 1; }
