// ι (#313): explicit template arguments name a specialization; a non-template is preferred over
// a template specialization of the same type, and of two template specializations the more
// specialized template wins ([over.over], [temp.arg.explicit], [temp.func.order]).
// EXIT: 0
template <class T> int t(T) { return 10; }
template <class T> int t(T, T) { return 20; }

int h(int) { return 1; }
template <class T> int h(T) { return 2; }

template <class T> int m(T) { return 3; }
template <class T> int m(T*) { return 4; }

int main() {
  int (*a)(int) = &t<int>;
  if (a(0) != 10) return 1;
  int (*b)(long, long) = t<long>;
  if (b(0, 0) != 20) return 2;
  // The explicit argument fixes T; the target picks between the two templates.
  int (*c)(char) = &t<char>;
  if (c('c') != 10) return 3;
  // [over.over]: the non-template h(int) beats the specialization h<int>.
  int (*d)(int) = h;
  if (d(0) != 1) return 4;
  // Only the template matches a long target.
  int (*e)(long) = &h;
  if (e(0) != 2) return 5;
  // Naming the specialization explicitly selects it.
  int (*g)(int) = h<int>;
  if (g(0) != 2) return 6;
  // m<int*> from m(T) and m<int> from m(T*) both have type int(int*): the more specialized wins.
  int (*k)(int*) = m;
  int x = 0;
  if (k(&x) != 4) return 7;
  int (*n)(int) = m;
  if (n(0) != 3) return 8;
  return 0;
}
