// ζ f1 (#88): one template used with many argument types, fundamental, pointer and class types
// (classes passed by reference), each use a separate specialization; member functions of the
// argument's class called from the template body ([temp.inst], [temp.deduct.call],
// [temp.res]).
// EXIT: 0
template<class T>
void swap_vals(T& a, T& b) {
  T t = a;
  a = b;
  b = t;
}

template<class T>
T* find_first(T* begin, T* end, const T& v) {
  for (T* p = begin; p != end; ++p)
    if (*p == v) return p;
  return end;
}

struct Rect {
  int w;
  int h;
  int width() const { return w; }
  int height() const { return h; }
};

struct Square {
  int side;
  int width() const { return side; }
  int height() const { return side; }
};

template<class T>
int area(const T& s) { return s.width() * s.height(); }

template<class T>
void grow(T& s) { s.w = s.w + 1; }

int main() {
  int a = 1, b = 2;
  swap_vals(a, b);
  if (a != 2 || b != 1) return 1;
  char c = 'x', d = 'y';
  swap_vals(c, d);
  if (c != 'y' || d != 'x') return 2;
  bool t = true, f = false;
  swap_vals(t, f);
  if (t || !f) return 3;
  long l = 10, m = 20;
  swap_vals(l, m);
  if (l != 20 || m != 10) return 4;
  int* pa = &a;
  int* pb = &b;
  swap_vals(pa, pb);
  if (pa != &b || pb != &a) return 5;
  Rect r1{1, 2}, r2{3, 4};
  swap_vals(r1, r2);
  if (r1.w != 3 || r2.h != 2) return 6;
  int xs[4] = {5, 6, 7, 8};
  if (find_first(xs, xs + 4, 7) != xs + 2) return 7;
  if (find_first(xs, xs + 4, 9) != xs + 4) return 8;
  char cs[3] = {'a', 'b', 'c'};
  if (find_first(cs, cs + 3, 'b') != cs + 1) return 9;
  if (area(r1) != 12) return 10;
  Square sq{5};
  if (area(sq) != 25) return 11;
  grow(r1);
  if (area(r1) != 16) return 12;
  return 0;
}
