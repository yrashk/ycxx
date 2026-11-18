// ζ c2 (#88): two template-ids name the same specialization when their type arguments are the
// same type and their non-type arguments have the same value after conversion to the
// parameter's type: Arr<2 + 3> is Arr<5>, U<5> is U<5u>, Letter<65> is Letter<'A'>, Box<signed>
// is Box<int>. So a function declared with one spelling and defined with the other is one
// function, and objects of either spelling bind to it ([temp.type], [temp.arg.nontype],
// [over.dcl], [basic.link]).
// EXIT: 0
template<int N>
struct Arr {
  int data[N];
};

template<unsigned N>
struct U {
  int get() const { return N; }
};

template<char C>
struct Letter {
  char get() const { return C; }
};

template<class T>
struct Box {
  T v;
};

int f(Arr<5>&);
int f(Arr<2 + 3>&) { return 5; }
int f(Arr<6>&) { return 6; }

int g(U<5>&);
int g(U<5u>&) { return 1; }

int h(Letter<'A'>&);
int h(Letter<65>&) { return 2; }

int k(Box<int>&);
int k(Box<signed>&) { return 3; }

int main() {
  Arr<5> a{};
  Arr<10 / 2> b{};
  Arr<6> c{};
  if (f(a) != 5) return 1;
  if (f(b) != 5) return 2;
  if (f(c) != 6) return 3;
  U<5> u{};
  if (g(u) != 1) return 4;
  if (u.get() != 5) return 5;
  Letter<'A'> l{};
  if (h(l) != 2) return 6;
  if (l.get() != 65) return 7;
  Box<int> x{0};
  if (k(x) != 3) return 8;
  Arr<5>* pa = &b;
  if (pa != &b) return 9;
  return 0;
}
