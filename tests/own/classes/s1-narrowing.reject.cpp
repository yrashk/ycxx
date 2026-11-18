// δ s1 (#86): narrowing conversions in list-initialization of aggregate members
// ([dcl.init.list] narrowing, [dcl.init.aggr]).
struct Small { char c; unsigned u; int i; };

int main() {
  long big = 100000;
  int neg = -1;
  Small a = {big, 1, 2}; //! error
  Small b{'a', neg, 2}; //! error
  Small c = {'a', 1, big}; //! error
  Small d = {300, 1, 2}; //! error
  Small ok = {'a', 1u, 7};  // constants that fit are not narrowing
  return a.i + b.i + c.i + d.i + ok.i;
}
