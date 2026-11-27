// #1417 [dcl.decl.general], [expr.prim.req], [expr.prim.req.nested], [temp.constr.atomic]: a
// trailing requires-clause written as a requires-expression (`requires requires ...`), with a
// simple, a type, a compound and a nested requirement; an invalid expression or type makes
// the constraint unsatisfied rather than ill-formed.
template<class A, class B> inline constexpr bool same = false;
template<class A> inline constexpr bool same<A, A> = true;
template<class A, class B> concept SameAs = same<A, B>;

struct Speaker {
  using tag = int;
  int speak() const { return 7; }
};
struct Chatty {
  using tag = char;
  long speak() const { return 8; }
};
struct Mute {};

template<class T>
struct Wrap {
  T t;
  int talk() const requires requires(const T& x) { x.speak(); } { return int(t.speak()); }
  int talk() const requires (!requires(const T& x) { x.speak(); }) { return -1; }

  static int tagged() requires requires { typename T::tag; requires sizeof(typename T::tag) == 4; } {
    return 1;
  }
  static int tagged() requires (!requires { typename T::tag; requires sizeof(typename T::tag) == 4; }) {
    return 0;
  }

  static int returns_int() requires requires(const T& x) { { x.speak() } -> SameAs<int>; } { return 1; }
  static int returns_int() requires (!requires(const T& x) { { x.speak() } -> SameAs<int>; }) { return 0; }
};

int main() {
  if (Wrap<Speaker>{}.talk() != 7) return 1;
  if (Wrap<Chatty>{}.talk() != 8) return 2;
  if (Wrap<Mute>{}.talk() != -1) return 3;
  if (Wrap<Speaker>::tagged() != 1) return 4;
  if (Wrap<Chatty>::tagged() != 0) return 5;
  if (Wrap<Mute>::tagged() != 0) return 6;
  if (Wrap<Speaker>::returns_int() != 1) return 7;
  if (Wrap<Chatty>::returns_int() != 0) return 8;
  if (Wrap<Mute>::returns_int() != 0) return 9;
  return 0;
}
