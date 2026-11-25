// #1153 [temp.expl.spec], [temp.inst]: an explicit specialization of a virtual member takes its
// slot, and a body that nothing requires is not instantiated with the class.
// EXIT: 0

template <class T> struct S {
  virtual int f() { return 1; }
  virtual int g() { return T::missing(); }
};
template <> int S<int>::f() { return 7; }
template <> int S<int>::g();
template <> int S<int>::g() { return 8; }

struct D : S<int> {
  int g() override { return 9 + S<int>::g(); }
};

static_assert(sizeof(S<char>) == sizeof(void*));  // its g cannot be instantiated
S<char>* never = nullptr;

int main() {
  S<int> s;
  S<int>& r = s;
  if (r.f() != 7 || r.g() != 8) return 1;
  D d;
  S<int>& q = d;
  return q.f() == 7 && q.g() == 17 && !never ? 0 : 2;
}
