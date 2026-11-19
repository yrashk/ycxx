// ζ m9 (#88, #314), from the step's review: members overload on template-ness, template-head and constraints
// ([temp.over.link]/4-6, [over.load]).
// EXIT: 0
template<class T> concept Small = sizeof(T) == 1;
struct S {
  template<class T> requires (sizeof(T) == 1) int f(T) { return 1; }
  template<class T> requires (sizeof(T) > 1) int f(T) { return 2; }
  template<Small T> int g(T) { return 3; }
  template<class T> int g(T) { return 4; }
  template<class T> int h(int) { return 5; }
  template<int N> int h(int) { return 6; }
  int k(int) { return 7; }
  template<class T> int k(int) { return 8; }
};
int main() { S s; return s.f('c') + s.f(1) + s.g('c') + s.g(1) + s.h<int>(0) + s.h<3>(0) + s.k(0) + s.k<int>(0) - 36; }
