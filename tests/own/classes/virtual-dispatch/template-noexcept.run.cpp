// #1153 [except.spec], [class.virtual]: a virtual member's exception specification depends on the
// class template's arguments.
// EXIT: 0

template <class T> struct A {
  virtual int f(T x) noexcept(noexcept(T(x)) && sizeof(T) > 1) { return int(x); }
  virtual int g() noexcept;
  virtual int h() noexcept(sizeof(T) > 1);
};
template <class T> int A<T>::g() noexcept { return 4; }
template <class T> int A<T>::h() noexcept(sizeof(T) > 1) { return 5; }

struct Throwing : A<char> {
  int f(char) override { return 2; }
  int h() override { return 6; }
};
struct Nonthrowing : A<int> {
  int f(int x) noexcept override { return 2 * x; }
};

int main() {
  Throwing t;
  A<char>& a = t;
  if (a.f('x') != 2 || a.g() != 4 || a.h() != 6) return 1;
  Nonthrowing n;
  A<int>& b = n;
  return b.f(21) == 42 && b.h() == 5 ? 0 : 2;
}
