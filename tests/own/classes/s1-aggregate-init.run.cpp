// δ s1 (#86): aggregate initialization with `= {…}` and `{…}`, fewer initializers than members
// (the rest copy-initialized from an empty initializer list, i.e. zero), `{}` and `= {}`, local
// array aggregate initialization ([dcl.init.aggr], [dcl.init.list], [dcl.init.general]).
// EXIT: 0
struct Triple {
  int a;
  long b;
  char c;
};

int main() {
  Triple t = {1, 2, 'x'};
  if (t.a != 1 || t.b != 2 || t.c != 'x') return 1;
  Triple u{4, 5, 'y'};
  if (u.a != 4 || u.b != 5 || u.c != 'y') return 2;
  Triple v = {7};
  if (v.a != 7 || v.b != 0 || v.c != 0) return 3;
  Triple w{};
  if (w.a != 0 || w.b != 0 || w.c != 0) return 4;
  Triple z = {};
  if (z.a != 0 || z.b != 0 || z.c != 0) return 5;
  Triple e{8, 9};
  if (e.a != 8 || e.b != 9 || e.c != 0) return 6;
  int arr[3] = {1, 2};
  if (arr[0] != 1 || arr[1] != 2 || arr[2] != 0) return 7;
  int zero[4] = {};
  if (zero[0] != 0 || zero[3] != 0) return 8;
  int n = 3;
  long m = n + 1;
  Triple r = {n * 2, m, 'b'};
  if (r.a != 6 || r.b != 4 || r.c != 'b') return 9;
  int sized[] = {5, 6, 7};
  if (sized[2] != 7) return 10;
  return 0;
}
