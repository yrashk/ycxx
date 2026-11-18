// ζ f1 (#88): a non-dependent name in a template body is looked up and bound where the template
// is defined: a better overload declared after the template is not considered when it is
// instantiated, while a call outside the template sees it ([temp.res], [temp.nondep]).
// EXIT: 0
int pick(long) { return 1; }

template<class T>
int use(T) { return pick(5); }

template<class T>
int use_other(T x) { return pick(5) * 10 + x; }

int pick(int) { return 2; }

int main() {
  if (use(0) != 1) return 1;
  if (use('c') != 1) return 2;
  if (use_other(3) != 13) return 3;
  if (pick(5) != 2) return 4;
  return 0;
}
