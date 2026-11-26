// [dcl.fct], [over.match.funcs], [temp.mem]
// EXIT: 12
struct Select {
  template <class U>
  int f() & { return 1; }
  template <class U>
  int f() && { return 2; }
};

int main() {
  Select s;
  return s.f<int>() * 10 + Select{}.f<int>();
}
