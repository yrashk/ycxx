// #306 [class.nest]/1: a class defined in a class is a member of it; it is named by a
// qualified-id outside and by its own name inside.
struct S {
  struct In {
    int w;
    int twice() const { return 2 * w; }
  };
  In i;
  In make(int v) const { return In{v}; }
};

int main() {
  S s{{3}};
  S::In j{4};
  S::In k = s.make(5);
  return s.i.w + j.w - 7 + k.twice() - 10;
}
